#define _BSD_SOURCE 1
//#define _GNU_SOURCE

#include <stdio.h>
#include <sys/socket.h>
#include <netdb.h>
#include <poll.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <net/ethernet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <termios.h>
#include <unistd.h>
#include <errno.h>

#include "kdp_serial.h"
#include "ip_sum.h"

#if !defined __linux__ || defined(HAVE_SIN_LEN)
    #ifdef INET_ADDRSTRLEN
        #define HAVE_SIN_LEN 1
    #else
        #define HAVE_SIN_LEN 0
    #endif
#endif

#define REVERSE_VIDEO   "\x1b[7m"
#define NORMAL_VIDEO    "\x1b[0m"

int opt_verbose = 0;
int g_linecount = 0;
static int g_ser;
static void serial_putc(char c)
{

    write(g_ser, &c, 1);

    if (g_linecount && !(g_linecount % 16)) {
        //fprintf(stderr, "\n    ");
        g_linecount = 0;
    } 

    if (opt_verbose) {
        if ((uint8_t)c == 0xfa) // start:
            ;//fprintf(stderr, "    ");
        else if ((uint8_t)c == 0xfb) { // stop:
            fprintf(stderr, "\n\n");
            g_linecount = 0;
        } else {
            fprintf(stderr, "%02x ", (uint8_t)c);
            g_linecount++;
        }
    }
}

// OS X's native poll function cannot handle TTY devices.
int working_poll(struct pollfd fds[], nfds_t nfds, int timeout)
{
    fd_set readfds;
    fd_set writefds;
    fd_set errorfds;
    int maxfd = 0;
    int i, r;

    struct timeval tv = { timeout / 1000, (timeout % 1000) * 10 };

    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    FD_ZERO(&errorfds);

     for (i = 0; i < nfds; ++i) {
        if (fds[i].fd + 1 > maxfd)
            maxfd = fds[i].fd + 1;

        if (fds[i].events & POLLIN) {
            FD_SET(fds[i].fd, &readfds);
        }

        if (fds[i].events & POLLOUT) {
            FD_SET(fds[i].fd, &writefds);
        }
    }

    r = select(maxfd, &readfds, &writefds, &errorfds, timeout != -1 ? &tv : NULL);
    if (r < 0) {
        // treat as timeout 
        if (errno == EAGAIN || errno == EINTR) 
            return 0;

        return r;
    }
    // timeout
    else if (r == 0) {
        return r;
    }

    r = 0;

    for (i = 0; i < nfds; ++i) {
        fds[i].revents = 0;

        if (FD_ISSET(fds[i].fd, &readfds))
            fds[i].revents |= POLLIN;

        if (FD_ISSET(fds[i].fd, &writefds))
            fds[i].revents |= POLLOUT;

        if (FD_ISSET(fds[i].fd, &writefds))
            fds[i].revents |= POLLERR;

        if (fds[i].revents != 0)
            ++r;
    }

    return r;
}

struct udp_ip_ether_frame_hdr {
    struct ether_header eh;
    struct ip ih;
    struct udphdr uh;
} __attribute__((packed));

union frame_t {
    uint8_t buf[1500];
    struct udp_ip_ether_frame_hdr h;
};
union frame_t frame;

/*!
  @abstract   The (fake) MAC address of the kernel's KDP
  @discussion
  This must be "serial" because the kernel won't allow anything else.
 */
u_char const client_macaddr[ETHER_ADDR_LEN] = { 's', 'e', 'r', 'i', 'a', 'l' };

/*!
  @abstract   The (fake) MAC address of our side of the KDP
  @discussion
  This can be anything really.  But it's more efficient to use characters that
  don't need to be escaped by kdp_serialize_packet.
 */
u_char const our_macaddr[ETHER_ADDR_LEN] = { 'f', 'o', 'o', 'b', 'a', 'r' };

/*!
  @abstract   The last IP sequence number.
 */
static uint16_t out_ip_id = 0;

/*!
  @abstract   A helper function to initialize the new UDP ethernet frame
  @argument   pFrame      Pointer to ethernet frame to initialize
  @argument   sAddr       Source IP address to use for packet, in network byte order
  @argument   sPort       Source UDP port to use for packet, in network byte order
  @argument   dataLen     Size of UDP data
 */
void setup_udp_frame(union frame_t *pFrame, struct in_addr sAddr, in_port_t sPort, ssize_t dataLen)
{
    memcpy(pFrame->h.eh.ether_dhost, client_macaddr, ETHER_ADDR_LEN);
    memcpy(pFrame->h.eh.ether_shost, our_macaddr, ETHER_ADDR_LEN);
    pFrame->h.eh.ether_type = htons(ETHERTYPE_IP);
    pFrame->h.ih.ip_v = 4;
    pFrame->h.ih.ip_hl = sizeof(struct ip) >> 2;
    pFrame->h.ih.ip_tos = 0;
    pFrame->h.ih.ip_len = htons(sizeof(struct ip) + sizeof(struct udphdr) + dataLen);
    pFrame->h.ih.ip_id = htons(out_ip_id++);
    pFrame->h.ih.ip_off = 0;
    pFrame->h.ih.ip_ttl = 60; // UDP_TTL from kdp_udp.c
    pFrame->h.ih.ip_p = IPPROTO_UDP;
    pFrame->h.ih.ip_sum = 0;
    pFrame->h.ih.ip_src = sAddr; // Already in NBO
    pFrame->h.ih.ip_dst.s_addr = 0xABADBABE; // FIXME: Endian.. little to little will be fine here.
    //pFrame->h.ih.ip_dst.s_addr = 0xBEBAADAB; // FIXME: Endian.. host big, target little (or vice versa)
    // Ultimately.. the address doesnt seem to actually matter to it.

    pFrame->h.ih.ip_sum = htons(~ip_sum((unsigned char *)&pFrame->h.ih, pFrame->h.ih.ip_hl));

    pFrame->h.uh.uh_sport = sPort; // Already in NBO
    pFrame->h.uh.uh_dport = htons(41139);
    pFrame->h.uh.uh_ulen = htons(sizeof(struct udphdr) + dataLen);
    pFrame->h.uh.uh_sum = 0; // does it check this shit?
}

int set_termopts(int fd)
{
    struct termios options;
    int rc;

    rc = 1;
    do {
        memset(&options, 0, sizeof(options));
        tcgetattr(fd, &options);

        if (-1 == cfsetispeed(&options, B115200)) {
            fprintf(stderr, "error, could not set baud rate\n");
            break;
        }

        if (-1 == cfsetospeed(&options, B115200)) {
            fprintf(stderr, "error, could not set baud rate\n");
            break;
        }

        options.c_iflag = 0;
        options.c_oflag = 0;
        options.c_cflag = CS8 | CREAD | CLOCAL;
        options.c_lflag = 0;
        options.c_cc[VMIN] = 1;
        options.c_cc[VTIME] = 5;

        tcflush(fd, TCIFLUSH);
        if (-1 == tcsetattr(fd, TCSANOW, &options)) {
            fprintf(stderr, "error, could not tcsetattr\n");
            break;
        }
        rc = 0;
    } while(0);

    return rc;
}


int main(int argc, char **argv)
{
    char *device_name;
    int s;

    device_name = NULL;
    while (1) {
        int c;

        c = getopt(argc, argv, "vd:");
        if (-1 == c)
            break;

        switch (c) {
            case 'v':
                opt_verbose = 1;
                break;
        }
    }

    if (argc > optind)
        device_name = argv[optind++];
    else
        device_name = "/dev/tty.usbserial-A40084Fi";

    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        fprintf(stderr, "Failed to open socket\n");
        return 1;
    }

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
#if HAVE_SIN_LEN
    saddr.sin_len = INET_ADDRSTRLEN;
#endif
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(41139);
    saddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(s, (struct sockaddr*)&saddr, sizeof(saddr)) != 0) {
        fprintf(stderr, "Failed to bind\n");
        return 1;
    }

    fprintf(stderr, "Opening %s\n", device_name);

    g_ser = open(device_name, O_RDWR | O_NONBLOCK | O_NOCTTY);
    if (-1 == g_ser) {
        fprintf(stderr, "Failed to open serial\n");
        return 1;
    }

    set_termopts(g_ser);
    fprintf(stderr, "Waiting for packets, pid=%lu\n", (long unsigned)getpid());

    struct pollfd pollfds[3] = {
        { s,            POLLIN, 0},
        { g_ser,        POLLIN, 0},
        { STDIN_FILENO, POLLIN, 0}
    };

    while (working_poll(pollfds, 3, -1) >= 0) {
        ssize_t bytesReceived = 0;

        if ((pollfds[0].revents & POLLIN) != 0 ) {
            struct sockaddr_in clientAddr;
            socklen_t cal = sizeof(clientAddr);
            bytesReceived = recvfrom(s, frame.buf + sizeof(frame.h), sizeof(frame) - sizeof(frame.h), 0, (struct sockaddr *) &clientAddr, &cal);
            in_port_t clntPort = ((struct sockaddr_in*)&clientAddr)->sin_port;

            if (opt_verbose)
                fprintf(stderr, "%ld bytes received from: %s\n", bytesReceived, inet_ntoa(clientAddr.sin_addr));

            setup_udp_frame(&frame, ((struct sockaddr_in*)&clientAddr)->sin_addr, clntPort, bytesReceived);
            kdp_serialize_packet(frame.buf, bytesReceived + sizeof(frame.h), &serial_putc);
            fflush(stderr);
        } else if ((pollfds[0].revents) != 0) {
            fprintf(stderr, "WTF?\n");
        }

        if ((pollfds[1].revents & POLLIN) != 0) {
            unsigned char c;

            if (read(g_ser, &c, 1) == 1) {
                unsigned int len = SERIALIZE_READING;
                union frame_t *pInputFrame = (void*)kdp_unserialize_packet(c, &len);
                if (pInputFrame != NULL) {
                    if (pInputFrame->h.ih.ip_p == 17) {
                        int nr;

                        size_t frameDataLen = len - sizeof(pInputFrame->h);

                        struct sockaddr_in clientAddr;
                        bzero(&clientAddr, sizeof(clientAddr));
#if HAVE_SIN_LEN
                        clientAddr.sin_len = INET_ADDRSTRLEN;
#endif
                        clientAddr.sin_family = AF_INET;
                        clientAddr.sin_port = pInputFrame->h.uh.uh_dport;
                        clientAddr.sin_addr = pInputFrame->h.ih.ip_dst;

                        nr = sendto(s, pInputFrame->buf + sizeof(pInputFrame->h), frameDataLen, 0, (struct sockaddr*)&clientAddr, sizeof(clientAddr));
#if 0
                        size_t udpDataLen = ntohs(pInputFrame->h.uh.uh_ulen) - sizeof(struct udphdr);
                        size_t ipDataLen = ntohs(pInputFrame->h.ih.ip_len) - sizeof(struct ip) - sizeof(struct udphdr);
                        fprintf(stderr, "Sent reply packet %d, %d, %d to UDP %d\n", (int)udpDataLen, (int)ipDataLen, (int)frameDataLen, (int)ntohs(pInputFrame->h.uh.uh_dport));
#endif
                    } else {
                        fprintf(stderr, "Discarding non-UDP packet proto %d of length %u\n", pInputFrame->h.ih.ip_p, len);
                    }
                } else {
                    if (len == SERIALIZE_WAIT_START) {
                        uint8_t b = c;

                        if ((b >= 0x80) || (b > 26 && b < ' ') ) {
                            printf(REVERSE_VIDEO "\\x%02x" NORMAL_VIDEO, b);
                            fflush(stdout);
                        } else if ((b <= 26) && (b != '\r') && (b != '\n') ) {
                            printf(REVERSE_VIDEO "^%c" NORMAL_VIDEO, b + '@'); // 0 = @, 1 = A, ..., 26 = Z
                            fflush(stdout);
                        } else
                            putchar(c);
                    }
                }
            }
        } else if ((pollfds[1].revents) != 0) {
            fprintf(stderr, "Shutting down serial input due to 0x%x\n", pollfds[1].revents);
            pollfds[1].events = 0;
        }

        if ((pollfds[2].revents & POLLIN) != 0) {
            int nw;
            uint8_t consoleBuf[1024];

            bytesReceived = read(pollfds[2].fd, consoleBuf, sizeof(consoleBuf));

            consoleBuf[bytesReceived-1] = '\0';
            fprintf(stderr, "%zd bytes received on console\n    \"%s\"\n\n", bytesReceived, consoleBuf);
            consoleBuf[bytesReceived-1] = '\n';

            nw = write(g_ser, consoleBuf, bytesReceived);
            if (-1 == nw) 
                fprintf(stderr, "error, unable to write to the serial port\n");
        } else if ((pollfds[2].revents) != 0) {
            fprintf(stderr, "Shutting down console input due to 0x%x\n", pollfds[2].revents);
            pollfds[2].events = 0;
        }
    }

    return 0;
}

