<html>
<head>
</head>
<body>
<h1>README for SerialKDPProxy</h1>

<p>Copyright 2009 David Elliott. All Rights Reserved.</p>

<h2>About SerialKDPProxy</h2>

<p>The SerialKDPProxy is used for KDP debugging over RS-232.  This is a debugger of <em>last resort</em>.  If
at all possible, use KDP over FireWire or KDP over Ethernet.  As far as I can tell, KDP over FireWire can
be made to work with both builtin (the default) and add-on (there is a kernel command-line option) FireWire
ports.  Be aware that if not on a Mac your FireWire port might physically be located on the motherboard but
the manufacturer of your board did not list it as being a built-in device in the ACPI tables.  Exhaust
all possibilities of getting the FireWire KDP kext to work on your target before attempting to use this.</p>

<p>Now, that said, the huge advantage to serial KDP is that you can attach to the kernel before IOKit has
started.  If you have a machine that fails before the ACPI PE has enumerated the PCI devices then this
is going to be your only option for examining the kernel state aside from predetermined debug print statements
present in the code.</p>

<h2>Using SerialKDPProxy</h2>

<h3>Configuring the kernel on the target</h3>

<p>See osfmk/kern/debug.h for the DB_* series of flags.  See osfmk/kdp/kdp_udp.h for how kdp_match_name is used.</p>

<p>First of all, to use the serial debugger you need kdp_match_name=serial in the boot args.  This causes the code
in kdp_init() to initialize the serial debugger.</p>
<p>Second, for effective use you probably want to set DB_HALT (0x1)
so the kernel will halt very early in startup, before even IOKit is attached.  Without this you won't be able
to attach to the kernel until after something has called the debug break function.  By setting DB_HALT the kernel
will call debug break and thus allow you to attach early.

<p>Third, you may want some other flags like DB_KPRT (0x8)
and DB_ARP (0x40).  As of now there is no way to send an ARP although if you format it correctly and push it
down the serial port (e.g. cat a file into the same /dev/tty* from another terminal) then the reply should be
logged to stderr as a non-UDP packet.  So for instance, debug=0x49 gives you DB_HALT, DB_KPRT and DB_ARP.</p>

<p>Lastly, you may or may not want serial=3 depending on whether or not you want to enable the serial console.
Obviously there are many more options like -v and io=XXX (see iokit/IOKit/IOKitDebug.h) that you may want depending
on what you are trying to do.  All of these are documented in various places by Apple and you can google most
of the flags to find out what they do (usually you'll find a page on Apple's site).</p>

<h3>Linking the machines together</h3>
<p>You will need a null modem cable.  Often these can be bought DB-9 female to DB-9 female with the null modem
circuitry integrated into the cable.  Other times you have a DB-9 female to DB-9 male cable with a null modem
adapter and a DB-9 female to female gender changer.  It is very rare these days to see DB-25 serial ports on
any machines these days although technically, DB-25 is the official RS-232 standard port.</p>

<p>On the target (machine whose kernel is being debugged) you must attach one end of your null-modem cable to
COM1.  The kernel has legacy COM1 0x3f8 IO base address hardcoded in it.  This is how the kernel is able to provide
kprintf and KDP over serial long before the IOKit has started.  Very recent motherboards are excluding a DB-9 port
on the ATX backplane but they almost always provide a 10-pin header (actually 9-pin because 1 is missing for key)
corresponding to COM1 (0x3f8).  The cables for these are not hard to come by and if you have been around long enough
and were smart enough to strip them off an older machine (e.g. 486 era) before sending it to the trash heap then you
probably have one.  All Xserve (as far as I'm aware) also provide serial on COM1 at the legacy IO base address and
the standard DB-9 male port is present on the exterior of the machine.</p>

<p>The other end of the null modem cable will ideally go to your host machine, that is the one you are going to be
running GDB on.  However, this does not have to be the case.  Theoretically you can perfectly well use a USB to
Serial adapter on the host side but in practice many of the OS X drivers for these seem to be absolute crap or
power cycling the target machine on the other end of the cable sends enough electrical noise down the serial line
that it just locks up.</p>

<p>As a workaround for these problems, SerialKDPProxy should build and run on Linux and hopefully other UNIX-like
operating systems.  Patches are encouraged if this is not the case.  If you do run SerialKDPProxy on a Linux machine
then instead of attaching GDB to localhost you'll attach GDB to the name or IP of your linux box.  In all cases GDB
itself runs on a Mac OS X machine you use as the debugger host</p>

<h3>Running SerialKDPProxy</h3>
<p>The SerialKDPProxy must be run on the machine with the serial port.  If on an OS X machine with a hardware serial
port then this will be /dev/tty.serial1.  If using a USB adapter then the name will be something else but it should
always be of the form /dev/tty.* like /dev/tty.usbserial1.  If you decide to run it on a Linux machine with a hardware
serial port then /dev/ttyS0 will usually be COM1 and /dev/ttyS1 will usually be COM2.</p>

<h3>Using GDB</h3>
<p>Your host machine (the machine you run GDB on) must be an OS X machine unless you have somehow managed to build
a cross-GDB (not for the faint of heart).  If the proxy is also running on the same machine then you will use
the command kdp-reattach localhost to attach to it.  If the proxy is running on a linux machine named "linuxbox"
then you'll want to kdp-reattach linuxbox.</p>

<p>For optimum debugging you want to download the OS X kernel debug kit (you can get this from ADC even with a free
ADC account).  You can basically follow the included instructions.  That is, cd /Volumes/KernelDebugKit.  Then
gdb ./mach_kernel.  Then from gdb source kgmacros.  Then kdp-reattach &lt;hostname or IP of proxy&gt; as described
above.</p>

<h3>Observations</h3>
<p>One thing to be aware of is that GDB has a tendency to basically lock up when it doesn't get the return packets
it wants from the kernel.  So you may run into situations where Ctrl+C does not work and you must kill your GDB
process.  This typically tends to happen when something has caused the kernel to fully lock up to the point where
it won't even properly panic itself.</p>

<p>One other thing to be aware of is that RS-232 isn't exactly an ideal communication medium.  Using the kgmacros
functions that dump various structures (e.g. things in the IORegistry) may very well take several minutes to
complete and there will be plenty of cases where GDB will send a packet and not receive a reply.  In general it
will resend the packet and the second time it will receive a reply. I don't believe this is a problem with
SerialKDPProxy but I could be wrong.</p>

<p>Part of the reason for this is that the target kernel will only communicate with the host when it is stopped.
There is no facility for stopping a running kernel and this is a general limitation of KDP, not specific to
running it over RS-232.  In general kernel debugging is only useful if you know what things you'd like to break
on.  Once the kernel has stopped on your breakpoint and has sent the machine state over to GDB you will be
able to examine kernel memory from this point.</p>

</body>
</html>
