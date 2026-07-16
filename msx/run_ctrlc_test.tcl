# Verify that Ctrl-C exits the complete-page player and restores DOS2.

set test_dir "/Volumes/EXT_SSD/AI/libkss_conversion_tools/tmp"
file mkdir $test_dir
set power on
set mute off
set master_volume 100
set PSG_volume 100
set SCC_volume 100
after realtime 20 {type "KSSPLAY.COM SCPZ.KSS 5"}
after realtime 23 {type "\r"}
after realtime 32 {
    keymatrixdown 6 0x02
    keymatrixdown 3 0x01
}
after realtime 33 {
    keymatrixup 3 0x01
    keymatrixup 6 0x02
}
after realtime 39 {
    set state [open [file join $test_dir salamander-ctrlc-state.txt] w]
    puts $state "PC=[format %04X [reg PC]]"
    puts $state "SP=[format %04X [reg SP]]"
    puts $state "PAGE0=[format %02X [debug read ioports 0xFC]]"
    puts $state "PAGE1=[format %02X [debug read ioports 0xFD]]"
    puts $state "PAGE2=[format %02X [debug read ioports 0xFE]]"
    puts $state "PAGE3=[format %02X [debug read ioports 0xFF]]"
    puts $state "A8=[format %02X [debug read ioports 0xA8]]"
    close $state
}
after realtime 42 {exit}
