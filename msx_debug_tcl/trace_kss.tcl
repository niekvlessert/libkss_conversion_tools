source "/Volumes/EXT_SSD/AI/libkss_conversion_tools/openmsx_debug/openmsx_trace.tcl"

set power on

after realtime 45 {type "KSPPLAY.COM F1SPIRIT.KSP 54"}
after realtime 48 {type "\r"}

after boot {
    trace_mapper none
    trace_pc_enabled off
    trace_disasm on
    trace_mem_ranges {{0x9800 0x98FF}}
    trace_io_ranges {{0xA8 0xA8} {0x98 0x9B} {0xA0 0xA1} {0xFC 0xFF}}
    trace_start {/Volumes/EXT_SSD/AI/libkss_conversion_tools/tmp/kssplay-slot-trace.log}
    after realtime 65 {
        set f [open "/Volumes/EXT_SSD/AI/libkss_conversion_tools/tmp/kssplay-return-dump.txt" w]
        foreach r {PC SP AF BC DE HL IX IY} {
            puts $f "$r=[format %04X [reg $r]]"
        }
        puts $f "F380=[binary encode hex [debug read_block memory 0xF380 0x20]]"
        puts $f "C561=[binary encode hex [debug read_block memory 0xC561 0x20]]"
        close $f
    }
    after realtime 80 {
        trace_stop
        exit
    }
}
