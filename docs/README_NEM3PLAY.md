# Nemesis 3 compressed KSS MSX player

In OpenMSX, mount this directory as drive A and run:

```basic
RUN "AUTOEXEC.BAS"
```

`NEM3COMP.KSS` is an MSX Disk-BASIC BLOAD image: it contains the exact compressed KSS payload at 4000H, followed by padding and the player at C000H. `NEM3RAW.KSS` is the unmodified KSS copy. The player expands the engine and RAM-mapper banks before INIT, then calls PLAY through H.TIMI. The machine must provide an MSX RAM mapper and the original KSS engine uses PSG/SCC hardware.
