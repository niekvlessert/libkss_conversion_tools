# Regression: Quarth must survive two live next-track changes.

set test_dir "/Volumes/EXT_SSD/AI/libkss_conversion_tools/tmp"
file mkdir $test_dir
set power on
set mute off
set master_volume 100
set PSG_volume 100
set SCC_volume 100
after realtime 12 {type "KSSPLAY.COM QCPZ.KSS 0"}
after realtime 15 {type "\r"}
after realtime 20 {soundlog start [file join $test_dir quarth-two-switches.wav]}
after realtime 26 {keymatrixdown 8 0x01}
after realtime 27 {keymatrixup 8 0x01}
after realtime 32 {keymatrixdown 8 0x01}
after realtime 33 {keymatrixup 8 0x01}
after realtime 40 {
    soundlog stop
    set state [open [file join $test_dir quarth-two-switches-state.txt] w]
    puts $state "PC=[format %04X [reg PC]]"
    puts $state "SP=[format %04X [reg SP]]"
    puts $state "PAGE1=[format %02X [debug read ioports 0xFD]]"
    puts $state "PAGE2=[format %02X [debug read ioports 0xFE]]"
    puts $state "PAGE3=[format %02X [debug read ioports 0xFF]]"
    close $state
    exit
}
