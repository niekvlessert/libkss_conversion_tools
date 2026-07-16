# Focused trace for the relocated Nemesis 3 runtime.
source "/Volumes/EXT_SSD/AI/libkss_conversion_tools/openmsx_debug/openmsx_trace.tcl"

after boot {
    trace_mapper none
    trace_pc_range 0x3FF0 0x4070
    trace_pc_enabled on
    trace_disasm on
    trace_mem_ranges {}
    trace_io_ranges {{0xA0 0xA1} {0xFC 0xFF}}
    trace_start {/tmp/nem3-runtime-trace.log}
    after realtime 10 {
        type "RUN \"AUTOEXEC.BAS\"\r"
        after realtime 20 {
            trace_stop
            exit
        }
    }
}
