set selector_snapshot "/Volumes/EXT_SSD/AI/libkss_conversion_tools/tmp/kssplays-openmsx-state.txt"
proc write_selector_snapshot {} {
    global selector_snapshot
    if {[catch {
        set snapshot "PC=[reg PC]\n"
        append snapshot "SP=[reg SP]\n"
        append snapshot "AF=[reg AF]\n"
        append snapshot "BC=[reg BC]\n"
        append snapshot "DE=[reg DE]\n"
        append snapshot "HL=[reg HL]\n"
        append snapshot "DISASM=[debug disasm]\n"
        set output [open $selector_snapshot w]
        puts -nonewline $output $snapshot
        close $output
    } message]} {
        puts stderr "selector snapshot failed: $message"
    }
    after realtime 0.25 write_selector_snapshot
}

after boot {
    write_selector_snapshot
}
