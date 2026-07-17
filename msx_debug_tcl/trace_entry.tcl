source "/Volumes/EXT_SSD/AI/libkss_conversion_tools/openmsx_debug/openmsx_trace.tcl"

set power on

after realtime 12 {type "KSPPLAY.COM F1SPIRIT.KSP 54"}
after realtime 15 {type "\r"}

after boot {
    trace_mapper none
    trace_pc_range 0xC000 0xC600
    trace_pc_enabled on
    trace_disasm on
    trace_mem_ranges {}
    trace_io_ranges {{0xA8 0xA8} {0xFC 0xFF}}
    trace_start {/Volumes/EXT_SSD/AI/libkss_conversion_tools/tmp/kssplay-entry-trace.log}
    after realtime 20 {
        trace_stop
        exit
    }
}
