source "/Volumes/EXT_SSD/AI/libkss_conversion_tools/openmsx_debug/openmsx_trace.tcl"

after boot {
    after realtime 13 {
        trace_mapper none
        trace_pc_enabled off
        trace_disasm on
        trace_mem_ranges {{0x9000 0x9000} {0x9800 0x98FF}}
        trace_io_ranges {{0xA0 0xA1} {0xFC 0xFF}}
        trace_start {/Volumes/EXT_SSD/AI/libkss_conversion_tools/tmp/f1-msx-scc-trace.log}
        after realtime 3 {
            trace_stop
            exit
        }
    }
}
