# Short OpenMSX trace for the MSX Nemesis 3 player.
# It records PSG/SCC writes, RAM-mapper writes, and player execution after AUTOEXEC.

source "/Volumes/EXT_SSD/AI/libkss_conversion_tools/openmsx_debug/openmsx_trace.tcl"

after boot {
    trace_mapper konami_scc
    trace_pc_range 0x5F00 0x6000
    trace_pc_enabled on
    trace_disasm on
    trace_mem_ranges {{0x9800 0x98FF} {0xB800 0xB8FF}}
    trace_io_ranges {{0x98 0x9B} {0xA0 0xA1} {0xC0 0xC7}}
    trace_start {/Volumes/EXT_SSD/AI/libkss_conversion_tools/tmp/nem3-audio-trace-fixed.log}
    after realtime 10 {
        type "RUN \"AUTOEXEC.BAS\"\r"
        after realtime 30 {
            trace_stop
            exit
        }
    }
}
