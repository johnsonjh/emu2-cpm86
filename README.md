EMU2: A simple text-mode x86 + DOS and CP/M-86 emulator
-------------------------------------------------------

This is a simple DOS emulator for the Linux text console, supporting basic DOS
and CP/M-86 system calls and console I/O.

Installation
------------

    make
    sudo make install

The above installs `emu2` into `$(DESTDIR)${PREFIX}/bin/emu2`, this is
`/usr/bin/emu2` by default.

Using the emulator
------------------

To run a DOS `.exe` or `.com` file, simply load it with

    emu2 myprog.exe

The emulator accepts some options in the command line and more options as
environment variables, this allows child process (programs run by your DOS
program) to inherit the configuration.

The full usage is:

    emu2 [options] <prog.exe> [args...] [-- environment vars]

Options (should be placed *before* the DOS program name):
- `-h`        Shows a brief help.

- `-b addr`   Load header-less binary at given address (to load ROMs or test data).

- `-r <seg>:<ip>`  Specify a run address to start execution (only for binary loaded data).

The available environment variables are:
- `EMU2_DEBUG_NAME`    Base name of a file to write the debug log, defaults to
                       the exe name if not given.

- `EMU2_DEBUG`         List of debug options to activate, from the following:
                       `cpu`, `int`, `port`, `dos`, `video`.

- `EMU2_PROGNAME`      DOS program name, if not given try to convert the unix
                       name to an equivalent DOS path.

- `EMU2_DEFAULT_DRIVE` DOS default (current) drive letter, if not given use `C:`

- `EMU2_CWD`           DOS current working directory, if not given tries to convert
                       the current directory to the equivalent DOS path inside the
                       DOS default drive, or `C:\` if not possible.

- `EMU2_DRIVE_`n       Set unix path as root of drive `n`, by default all drives
                       point to the unix working directory.

- `EMU2_APPEND`        Sets a list of paths to search for data files on open,
                       emulating the DOS `APPEND` command. Only files with a
                       relative path are included in the search, and the search
                       is relative to the current working directory if no drive
                       letter is specified in the append path.
                       For example, if set to "`TXT;C:\IN`", when opening the
                       file "`CAT.TXT`" the file is searched as "`CAT.TXT`",
                       "`TXT\CAT.TXT`" and "`C:\IN\CAT.TXT`" in turn.

- `EMU2_CODEPAGE`      Set DOS code-page to the specified string. Set to '?' to
                       show list of included code-pages, multiple aliases
                       separated with commas.  Set to a file name to read the
                       mapping table from a file with the unicode value for
                       each byte.  You can download mapping tables from
                       ftp://ftp.unicode.org/Public/MAPPINGS/VENDORS/MICSFT/PC/
                       The code-page setting affects keyboard input and screen
                       output, but does not change the DOS NLS information.
                       The default code-page is CP437.

- `EMU2_LOWMEM`        Limits main memory to 512KB, this fixes some old DOS
                       programs with a bug that checks available memory using
                       "signed" comparison instructions (JLE instead of JBE).
                       This is needed at least for MASM versions 1.0 and 1.10.

- `EMU2_DOSVER`        Changes the reported DOS version, allowing programs that
                       checks this version to run.  You can specify a major
                       version or a major dot minor, for example "3.20", "2.11"
                       or "5".

- `EMU2_ROWS`          Setups the VGA text mode to the given number of rows,
                       from 12 to 50 at the program start. Some full-screen DOS
                       programs will retrieve this info and adjust the screen
                       properly, some other will ignore this and setup the text
                       mode again.

- `EMU2_CPU_SPEED`     Limits the emulated CPU speed to at most the given
                       number of instructions per millisecond. For reference,
                       a value of 1000 (1 MIPS) approximates a fast 8086 or
                       slow 80286. By default (or when set to 0), there is no
                       limit; a modern PC can typically reach 200,000 or more
                       instructions per millisecond. Note that this does not
                       accurately emulate a specific CPU speed, since real
                       8086/80286 processors take a varying number of cycles
                       per instruction.

- `EMU2_CPM_DISK`      Block size of the fabricated CP/M-86 "disk" that presents
                       each drive's host directory: `auto` (default), `1k`, `2k`,
                       `4k`, `8k` or `16k`. The block size is the allocation
                       granularity -- the smallest reportable file size (`1k` keeps
                       small files exact; bigger blocks are needed for bigger
                       disks). `auto` scans the directory and picks the smallest
                       block size that holds it. The disk itself is sized to the
                       directory's contents (see `EMU2_CPM_FREE`) and then capped at
                       the guest-tool ceiling: about 8 MB for a standard CP/M 2.2
                       program (its 16-bit record count and allocation bitmap top
                       out there), or 512 MB with `EMU2_CPM_PLUS`. Files too big to
                       fit aren't listed, as on a real CP/M disk. Per-drive override:
                       `EMU2_CPM_DISK_C`, `EMU2_CPM_DISK_D`, ... take precedence over
                       the global setting. Only affects native CP/M-86 programs.

- `EMU2_CPM_FREE`      Target percentage of free space the `auto` disk sizing aims
                       to leave (default `25`, clamped to 0..90).

- `EMU2_CPM_PLUS`      When set, use CP/M 3 (Personal CP/M-86 / CP/M-86 Plus)
                       limits: up to 2048 extents per file (32 MB files) and 512 MB
                       disks, instead of the standard 512 extents (8 MB files, ~8 MB
                       disks). Needs CP/M-3-aware tools to use the larger sizes.

- `EMU2_CPMVER`        CP/M version reported to native CP/M-86 programs by BDOS
                       function 12, as a major dot minor (e.g. `3.1`, `2.2`) or a
                       bare major (`3`). Defaults to `3.1`. CP/M 3.0 and later
                       maintain the *last-record byte count* (LRBC) in the
                       directory/FCB `S1` field, so programs see exact file lengths
                       instead of sizes rounded up to the 128-byte record boundary.
                       Older programs that expect 2.2 treat `S1` as reserved, so the
                       byte-count metadata is only filled in when a 3.0+ version is
                       reported; set this to `2.2` for programs that misbehave when
                       told they are running under CP/M 3.

- `EMU2_LRBC_NOTRUNC`  By default, when LRBC is active (CP/M 3.0+), emu2 trims a
                       host file to its exact byte count on close, so output is no
                       longer rounded up to a whole 128-byte record. Set this
                       (to any value other than `0`/`off`/`no`/`false`) to keep
                       files padded to the record boundary while still reporting
                       the byte count via `S1`. Has no effect under CP/M 2.2,
                       which has no LRBC and is never trimmed.

- `EMU2_CPM_ISXLRBC`   Selects how the last-record byte count in `S1` is
                       interpreted. There is no universally agreed meaning, so two
                       conventions exist. By default emu2 follows Digital Research's
                       DOS Plus / Personal CP/M-86: `S1` is the number of bytes
                       *used* in the last record. Set this to use the ISX-style
		       (ISIS-II emulator) convention instead, where `S1` is the
		       number of *unused* bytes in the last record. In both
		       conventions `S1` of `0` means the file fills its last record
		       exactly. Has no effect under CP/M 2.2, which has no LRBC.

- `EMU2_VT52`          Native CP/M-86 programs drive the console as a DOS-PLUS
                       (CP/M-86 4.1) terminal. emu2 interprets the console control
                       sequences -- VT52 cursor/erase codes, DRI colour codes
                       (`ESC b`/`ESC c`), and ANSI/VT100 codes -- and applies them
                       to the emulated PC screen, or, for programs that never touch
                       the BIOS video and so talk straight to the host terminal,
                       translates them to the ANSI your terminal understands. On by
                       default; set to `0`/`off`/`no` to disable. No effect on DOS.

Simple Example
--------------

For a simple example, we can run the Turbo Pascal 3, available from the antique
software collection as a file `tp302.zip`.

First, make a new directory and unzip the file:

    $ mkdir tp302
    $ cd tp302
    $ unzip ../tp302.zip
    $ ls
    ACCESS3.BOX   CALC.PAS      DEMO-BCD.PAS  GRAPH.P     SUBDIR.PAS    TURBO.COM
    ART.PAS       CMDLINE.PAS   EXTERNAL.DOC  LISTER.PAS  TINST.COM     TURBO.MSG
    CALCDEMO.MCS  COLOR.PAS     GETDATE.PAS   README      TINST.MSG     TURTLE.PAS
    CALC.HLP      DEMO1-87.PAS  GETTIME.PAS   README.COM  TURBO-87.COM  WINDOW.PAS
    CALC.INC      DEMO2-87.PAS  GRAPH.BIN     SOUND.PAS   TURBOBCD.COM
    $ chmod +w *

The last is necessary as you will want to to modify the program files afterwards.

The main file is named "TURBO.COM", there is also a "README.COM" to read further
info, try that:

    $ emu2 README.COM

![Image of README.COM](doc/readme.com.png)

You can press exit to return to the command line. Now, you can configure the compiler,
as the README said, just load `TINST.COM`, press "S" (screen type), "0" (default), "N"
(screen does not blink) and "Q" to quit.

    $ emu2 TINST.COM

                       TURBO Pascal installation menu.
                 Choose installation item from the following:

    [S]creen type |  [C]ommand installation  |  [M]sg file path  |  [Q]uit


                             Enter S, C, M or Q: 

    Choose one of the following displays:

      0)  Default display mode
      1)  Monochrome display
      2)  Color display 80x25
      3)  Color display 40x25
      4)  b/w   display 80x25
      5)  b/w   display 40x25

    Which display? (Enter no. or ^Q to exit):

    Does your screen blink when the text scrolls? (Y/N):

Finally, we are ready to run the program:

    $ emu2 TURBO.COM
    ----------------------------------------
    TURBO Pascal system        Version 3.02A
                                      PC-DOS
                                            
    Copyright (C) 1983,84,85,86 BORLAND Inc.
    ----------------------------------------
                                            
    Default display mode                    
                                            
                                            
                                            
    Include error messages (Y/N)?           

    Logged drive: C                         
    Active directory: \                     
                                            
    Work file:                              
    Main file:                              
                                            
    Edit     Compile  Run   Save            
                                            
    Dir      Quit  compiler Options         
                                            
    Text:     0 bytes                       
    Free: 62024 bytes                       
                                            
    >                                       

Try loading a program, use "e" to edit, type "window.pas", and you are in the editor:

          Line 1    Col 1   Insert    Indent  C:WINDOW.PAS                          
    program TestWindow;                                                             
    {$C-}                                                                           
    {                                                                               
                  WINDOW DEMONSTRATION PROGRAM  Version 1.00A                       
                                                                                    
           This program demonstrates the use of windows on the IBM PC               
           and true compatibles.                                                    
                                                                                    
           PSEUDO CODE                                                              
           1.  MakeWindow        - draws window boxes on the screen                 
           2.  repeat                                                               
                 UpdateWindow 1  - scrolls the window contents up or                
                                   down for each window.                            
                 UpdateWindow 2                                                     
                 UpdateWindow 3                                                     
               until a key is pressed                                               
           3.  Reset to full screen window                                          
                                                                                    
           INSTRUCTIONS                                                             
           1.  Compile this program using the TURBO.COM compiler.                   
           2.  Type any key to exit the program.                                    
    }                                                                               
                                                                                    
                                                                                    

To exit the editor, type "CONTROL+K" and "D", in the prompt you can now type "R" to
compile and run the program.

![Image of WINDOW.PAS running](doc/window.pas.png)

Advanced Example
----------------

For a more advanced example, we can install and run Turbo Pascal 5.5, available from the same
antique software collection as a file `tp55.zip`.

First, make a new directory and unzip the file:

    $ mkdir tp55
    $ cd tp55
    $ unzip ../tp55.zip
    $ ls
    Disk1 Disk2

As you see, the program was distributed in two disks, and must be installed before running.

To install, let's first copy all the contents to one directory:

    $ mkdir all
    $ cp -r Disk1/* Disk2/* all/

And now, run the emulator giving the correct paths to simulate a floppy drive:

    $ EMU2_DEFAULT_DRIVE=A EMU2_DRIVE_A=all emu2 all/INSTALL.EXE

![Image of TP55 INSTALL.EXE](doc/tp55.inst-1.png)

Type enter, enter again to install from drive "A", again to install on a hard-drive, go down
to "Start Installation" and enter again. The install program shows an error, this is because
we copied all the content to one drive. Simply type "S" to skip all errors.

After the installation is finished, we must run the install again, to copy the missing files
from before, with the same command line:

    $ EMU2_DEFAULT_DRIVE=A EMU2_DRIVE_A=all emu2 all/INSTALL.EXE

![Image of TP55 INSTALL.EXE at the end](doc/tp55.inst-2.png)

Again, press enter to the questions and go to "Start Installation", this time will complete
without errors.

You can now compile from the command line, as:

    $ emu2 tp/tpc.exe -- 'PATH=C:\TP'
    Turbo Pascal Version 5.5  Copyright (c) 1983,89 Borland International
    Syntax: TPC [options] filename [options]
    /B	Build all units		/$A-	No word alignment
    /Dxxx	Define conditionals	/$B+	Complete boolean evaluation
    /Exxx	EXE & TPU directory	/$D-	No debug information
    /Fxxx	Find run-time error	/$E-	No 8087 emulation
    /GS	Map file with segments	/$F+	Force FAR calls
    /GP	Map file with publics	/$I-	No I/O checking
    /GD	Detailed map file	/$L-	No local debug symbols
    /Ixxx	Include directories	/$Mxxx	Memory allocation parameters
    /L	Link buffer on disk	/$N+	8087 code generation
    /M	Make modified units	/$O+	Overlays allowed
    /Oxxx	Object directories	/$R+	Range checking
    /Q	Quiet compile		/$S-	No stack checking
    /Txxx	Turbo directories	/$V-	No var-string checking
    /Uxxx	Unit directories
    /V	EXE debug information

    $ emu2 tp/tpc.exe tp\\qsort.pas -- 'PATH=C:\TP'
    Turbo Pascal Version 5.5  Copyright (c) 1983,89 Borland International
    TP\QSORT.PAS(66)
    66 lines, 4384 bytes code, 2668 bytes data.

    $ emu2 tp/qsort.exe
    ....

And for the IDE, you can use:

    emu2 tp/turbo.exe  -- 'PATH=C:\TP'

![Image of TP55 environment](doc/tp55.turbo.png)

