source "/Volumes/EXT_SSD/AI/libkss_conversion_tools/openmsx_debug/openmsx_trace.tcl"

after boot {
    trace_mapper none
    trace_pc_range 0x4FF0 0x5010
    trace_pc_enabled on
    trace_disasm on
    trace_mem_ranges {}
    trace_io_ranges {{0x98 0x9B} {0xA0 0xA1} {0xFC 0xFF}}
    trace_start {/Volumes/EXT_SSD/AI/libkss_conversion_tools/tmp/f1-msx-audio-trace.log}
    after realtime 30 {
        trace_stop
        exit
    }
}
