# OpenMSX smoke test for the uncompressed, logical-map Quarth KSS.

source "/Volumes/EXT_SSD/AI/libkss_conversion_tools/openmsx_debug/openmsx_trace.tcl"
set test_dir "/Volumes/EXT_SSD/AI/libkss_conversion_tools/tmp"
file mkdir $test_dir

set power on
# Make the visible smoke test audible even when openMSX persisted a muted
# or zero-volume setting from a previous GUI session.
set mute off
set master_volume 100
set PSG_volume 100
set SCC_volume 100
after realtime 12 {type "KSSPLAY.COM QUARTH.KSS 15"}
after realtime 15 {type "\r"}
after realtime 25 {
    soundlog start [file join $test_dir quarth-msx.wav]
}
after realtime 18 {
    set handoff [open [file join $test_dir quarth-msx-handoff.txt] w]
    puts $handoff "PC=[format %04X [reg PC]]"
    puts $handoff "SP=[format %04X [reg SP]]"
    puts $handoff "MAP_FD=[format %02X [debug read ioports 0xFD]]"
    puts $handoff "MAP_FE=[format %02X [debug read ioports 0xFE]]"
    puts $handoff "MAP_FF=[format %02X [debug read ioports 0xFF]]"
    puts $handoff "CURRENT_C000=[binary encode hex [debug read_block memory 0xC000 0x20]]"
    puts $handoff "CURRENT_52E4=[binary encode hex [debug read_block memory 0x52E4 0x20]]"
    puts $handoff "CURRENT_5C47=[binary encode hex [debug read_block memory 0x5C47 0x50]]"
    puts $handoff "CURRENT_6000=[binary encode hex [debug read_block memory 0x6000 0x20]]"
    puts $handoff "SEGMENT_00_HEADER=[binary encode hex [debug read_block {Main RAM} 0x0916 0x20]]"
    puts $handoff "SEGMENT_00_STATE=[binary encode hex [debug read_block {Main RAM} 0x0936 0x28]]"
    puts $handoff "SEGMENT_00_MARKER=[binary encode hex [debug read_block {Main RAM} 0x0C9F 1]]"
    close $handoff
}
after realtime 30 {
    set early [open [file join $test_dir quarth-msx-early.txt] w]
    puts $early "PC=[format %04X [reg PC]]"
    puts $early "SP=[format %04X [reg SP]]"
    puts $early "SLOT_A8=[format %02X [debug read ioports 0xA8]]"
    set ffff_raw [debug read memory 0xFFFF]
    puts $early "SLOT_FFFF_RAW=[format %02X $ffff_raw]"
    puts $early "SLOT_FFFF_AFTER_CPL=[format %02X [expr {$ffff_raw ^ 0xFF}]]"
    puts $early "RAMAD0=[format %02X [debug read memory 0xF341]]"
    puts $early "RAMAD1=[format %02X [debug read memory 0xF342]]"
    puts $early "RAMAD2=[format %02X [debug read memory 0xF343]]"
    puts $early "RAMAD3=[format %02X [debug read memory 0xF344]]"
    puts $early "MAP_FC=[format %02X [debug read ioports 0xFC]]"
    puts $early "MAP_FD=[format %02X [debug read ioports 0xFD]]"
    puts $early "MAP_FE=[format %02X [debug read ioports 0xFE]]"
    puts $early "MAP_FF=[format %02X [debug read ioports 0xFF]]"
    puts $early "CURRENT_C000=[binary encode hex [debug read_block memory 0xC000 0x20]]"
    puts $early "CURRENT_52E4=[binary encode hex [debug read_block memory 0x52E4 0x20]]"
    puts $early "CURRENT_5C47=[binary encode hex [debug read_block memory 0x5C47 0x50]]"
    puts $early "CURRENT_6000=[binary encode hex [debug read_block memory 0x6000 0x20]]"
    puts $early "CURRENT_C960=[binary encode hex [debug read_block memory 0xC960 0x50]]"
    puts $early "CURRENT_4000=[binary encode hex [debug read_block memory 0x4000 0x20]]"
    puts $early "CURRENT_8000=[binary encode hex [debug read_block memory 0x8000 0x20]]"
    puts $early "CURRENT_A8=[format %02X [debug read ioports 0xA8]]"
    puts $early "SEGMENT_00_C000=[binary encode hex [debug read_block {Main RAM} 0x0000 0x20]]"
    puts $early "SEGMENT_03_RUNTIME_SOURCE=[binary encode hex [debug read_block {Main RAM} [expr {3 * 0x4000 + 0x1465}] 0x20]]"
    puts $early "SEGMENT_02_RUNTIME_TARGET=[binary encode hex [debug read_block {Main RAM} [expr {2 * 0x4000 + 0x12E4}] 0x20]]"
    puts $early "SEGMENT_02_RUNTIME_CONFIG=[binary encode hex [debug read_block {Main RAM} [expr {2 * 0x4000 + 0x1C47}] 0x20]]"
    puts $early "SEGMENT_02_RUNTIME_MARKER=[binary encode hex [debug read_block {Main RAM} [expr {2 * 0x4000 + 0x2232}] 2]]"
    puts $early "SEGMENT_00_C960=[binary encode hex [debug read_block {Main RAM} 0x0960 0x50]]"
    puts $early "SEGMENT_00_D104=[binary encode hex [debug read_block {Main RAM} 0x1104 0x0B]]"
    puts $early "SEGMENT_00_MARKER=[binary encode hex [debug read_block {Main RAM} 0x0C9F 1]]"
    puts $early "SEGMENT_03_0028=[binary encode hex [debug read_block {Main RAM} [expr {3 * 0x4000 + 0x28}] 8]]"
    puts $early "SEGMENT_00_HEADER=[binary encode hex [debug read_block {Main RAM} 0x0916 0x20]]"
    puts $early "SEGMENT_00_STATE=[binary encode hex [debug read_block {Main RAM} 0x0936 0x28]]"
    puts $early "SEGMENT_03_0028=[binary encode hex [debug read_block {Main RAM} [expr {3 * 0x4000 + 0x28}] 8]]"
    close $early
}
after realtime 70 {
    soundlog stop
    set state [open [file join $test_dir quarth-msx-state.txt] w]
    puts $state "PC=[format %04X [reg PC]]"
    puts $state "SP=[format %04X [reg SP]]"
    puts $state "PAGE1=[format %02X [debug read ioports 0xFD]]"
    puts $state "PAGE2=[format %02X [debug read ioports 0xFE]]"
    puts $state "PAGE3=[format %02X [debug read ioports 0xFF]]"
    puts $state "A8=[format %02X [debug read ioports 0xA8]]"
    puts $state "MEM_4000=[binary encode hex [debug read_block memory 0x4000 0x20]]"
    puts $state "MEM_8000=[binary encode hex [debug read_block memory 0x8000 0x20]]"
    puts $state "VECTOR_0028=[binary encode hex [debug read_block memory 0x0028 8]]"
    puts $state "MAIN0_VECTOR=[binary encode hex [debug read_block {Main RAM} [expr {3 * 0x4000 + 0x28}] 8]]"
    puts $state "PLAYER_CONFIG=[binary encode hex [debug read_block memory 0x5C47 0x50]]"
    puts $state "SEGMENT_02_RUNTIME_TARGET=[binary encode hex [debug read_block {Main RAM} [expr {2 * 0x4000 + 0x12E4}] 0x20]]"
    puts $state "SEGMENT_02_RUNTIME_CONFIG=[binary encode hex [debug read_block {Main RAM} [expr {2 * 0x4000 + 0x1C47}] 0x20]]"
    puts $state "SEGMENT_02_RUNTIME_MARKER=[binary encode hex [debug read_block {Main RAM} [expr {2 * 0x4000 + 0x2232}] 2]]"
    puts $state "PLAYER_MARKERS=[binary encode hex [debug read_block {Main RAM} [expr {0x1104}] 0x0B]]"
    puts $state "RESIDENT_BANK_HANDLER=[binary encode hex [debug read_block memory 0x5A25 0x20]]"
    set mainram_size [debug size {Main RAM}]
    puts $state "MAIN_RAM_SIZE=$mainram_size"
    set segments [expr {$mainram_size / 0x4000}]
    for {set segment 0} {$segment < $segments} {incr segment} {
        set head [debug read_block {Main RAM} [expr {$segment * 0x4000}] 0x20]
        puts $state "SEGMENT_[format %02X $segment]_HEAD=[binary encode hex $head]"
    }
    close $state
    exit
}
