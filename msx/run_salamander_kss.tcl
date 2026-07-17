# Visible OpenMSX smoke test for compressed Salamander complete-page track 5.

set test_dir "/Volumes/EXT_SSD/AI/libkss_conversion_tools/tmp"
file mkdir $test_dir
set power on
set mute off
set master_volume 100
set PSG_volume 100
set SCC_volume 100

after realtime 20 {type "KSSPLAY.COM SALAMAND.KSS 5"}
after realtime 23 {type "\r"}
after realtime 28 {
    set screen [open [file join $test_dir salamander-track5-screen.bin] wb]
    puts -nonewline $screen [debug read_block VRAM 0 0x4000]
    close $screen
}
after realtime 30 {
    soundlog start [file join $test_dir salamander-track5-msx.wav]
}
after realtime 36 {keymatrixdown 8 0x01}
after realtime 37 {keymatrixup 8 0x01}
after realtime 50 {
    soundlog stop
    set state [open [file join $test_dir salamander-track5-msx-state.txt] w]
    puts $state "PC=[format %04X [reg PC]]"
    puts $state "SP=[format %04X [reg SP]]"
    puts $state "PAGE1=[format %02X [debug read ioports 0xFD]]"
    puts $state "PAGE2=[format %02X [debug read ioports 0xFE]]"
    puts $state "PAGE3=[format %02X [debug read ioports 0xFF]]"
    puts $state "A8=[format %02X [debug read ioports 0xA8]]"
    puts $state "MILESTONE=[binary encode hex [debug read_block memory 0xD8F0 0x10]]"
    puts $state "ENGINE_6000=[binary encode hex [debug read_block memory 0x6000 0x20]]"
    puts $state "WORK_7E00=[binary encode hex [debug read_block memory 0x7E00 0x20]]"
    close $state
    exit
}
