# openmsx_trace.tcl
# openMSX 21.x tracing helper:
#   - instruction PC + disassembly
#   - selected memory-write ranges
#   - MSX memory-mapper writes on ports FC-FF
#   - cartridge mapper writes for common mapper types
#
# Load with:
#   openmsx -script /path/to/openmsx_trace.tcl
#
# Console commands:
#   trace_start ?filename?
#   trace_stop
#   trace_status
#   trace_pc_range 0x4000 0xBFFF
#   trace_mem_ranges {{0xC000 0xFFFF}}
#   trace_mapper konami_scc
#   trace_disasm on|off

namespace eval ::omtrace {
    variable active 0
    variable fh ""
    variable ids {}

    # Defaults. PC tracing is deliberately unrestricted; narrow this range
    # when possible because tracing every instruction is expensive.
    variable pc_first 0x0000
    variable pc_last  0xFFFF
    variable mem_ranges {{0xC000 0xFFFF}}
    variable io_ranges {}
    variable mapper_profile "konami_scc"
    variable include_disasm 1
    variable trace_pc_enabled 1

    variable line_count 0
    variable current_pc 0
    variable default_directory [file normalize "~/openmsx-trace"]
}

proc ::omtrace::hex4 {value} {
    return [format "%04X" [expr {$value & 0xFFFF}]]
}

proc ::omtrace::hex2 {value} {
    return [format "%02X" [expr {$value & 0xFF}]]
}

proc ::omtrace::now {} {
    return [machine_info time]
}

proc ::omtrace::write_line {line} {
    variable fh
    variable line_count
    if {$fh eq ""} {
        return
    }
    puts $fh $line
    incr line_count
}

proc ::omtrace::mapper_ranges {profile} {
    switch -- $profile {
        none {
            return {}
        }
        konami_scc {
            return {
                {0x5000 0x57FF}
                {0x7000 0x77FF}
                {0x9000 0x97FF}
                {0xB000 0xB7FF}
            }
        }
        konami {
            return {
                {0x6000 0x7FFF}
                {0x8000 0x9FFF}
                {0xA000 0xBFFF}
            }
        }
        ascii8 {
            return {
                {0x6000 0x7FFF}
            }
        }
        ascii16 {
            return {
                {0x6000 0x67FF}
                {0x7000 0x77FF}
            }
        }
        generic {
            # Useful for unknown mappers, but potentially noisy.
            return {
                {0x4000 0xBFFF}
            }
        }
        default {
            error "Unknown mapper profile '$profile'; use none, konami_scc, konami, ascii8, ascii16, or generic"
        }
    }
}

proc ::omtrace::is_mapper_address {address} {
    variable mapper_profile
    foreach range [mapper_ranges $mapper_profile] {
        lassign $range first last
        if {$address >= $first && $address <= $last} {
            return 1
        }
    }
    return 0
}

proc ::omtrace::mapper_register_name {profile address value} {
    switch -- $profile {
        konami_scc {
            if {$address >= 0x5000 && $address <= 0x57FF} { return "BANK0" }
            if {$address >= 0x7000 && $address <= 0x77FF} { return "BANK1" }
            if {$address >= 0x9000 && $address <= 0x97FF} {
                if {($value & 0x3F) == 0x3F} {
                    return "BANK2/SCC_ENABLE"
                }
                return "BANK2"
            }
            if {$address >= 0xB000 && $address <= 0xB7FF} { return "BANK3" }
        }
        konami {
            if {$address >= 0x6000 && $address <= 0x7FFF} { return "BANK1" }
            if {$address >= 0x8000 && $address <= 0x9FFF} { return "BANK2" }
            if {$address >= 0xA000 && $address <= 0xBFFF} { return "BANK3" }
        }
        ascii8 {
            if {$address >= 0x6000 && $address <= 0x7FFF} {
                return [format "BANK%d" [expr {($address - 0x6000) >> 11}]]
            }
        }
        ascii16 {
            if {$address >= 0x6000 && $address <= 0x67FF} { return "BANK0_16K" }
            if {$address >= 0x7000 && $address <= 0x77FF} { return "BANK1_16K" }
        }
        generic {
            return "UNKNOWN_REGISTER"
        }
    }
    return "MAPPER"
}

proc ::omtrace::on_pc {} {
    variable include_disasm
    variable current_pc

    set pc [reg PC]
    set current_pc $pc
    set t [now]

    if {$include_disasm} {
        if {[catch {set instruction [lindex [debug disasm $pc] 0]}]} {
            set instruction "<disassembly unavailable>"
        }
        write_line [format "PC      %.9f PC=%04X  %s" $t $pc $instruction]
    } else {
        write_line [format "PC      %.9f PC=%04X" $t $pc]
    }
}

proc ::omtrace::on_mem_write {} {
    variable current_pc

    set current_pc [reg PC]
    set address $::wp_last_address
    set value   $::wp_last_value

    # Cartridge mapper ranges have their own watchpoints and log records.
    if {[is_mapper_address $address]} {
        return
    }

    write_line [format "MEM     %.9f PC=%04X ADDR=%04X VALUE=%02X" \
        [now] $current_pc $address $value]
}

proc ::omtrace::on_cart_mapper_write {} {
    variable current_pc
    variable mapper_profile

    set current_pc [reg PC]
    set address $::wp_last_address
    set value   $::wp_last_value
    set regname [mapper_register_name $mapper_profile $address $value]

    write_line [format "CARTMAP %.9f PC=%04X ADDR=%04X VALUE=%02X PROFILE=%s REG=%s" \
        [now] $current_pc $address $value $mapper_profile $regname]
}

proc ::omtrace::on_ram_mapper_write {} {
    variable current_pc

    set current_pc [reg PC]
    # openMSX exposes the full Z80 I/O address here.  For OUT (C),A the
    # high byte is whatever was in B; the actual hardware port is A0.
    set raw_port $::wp_last_address
    set port  [expr {$raw_port & 0xFF}]
    set value $::wp_last_value
    set page  [expr {$port - 0xFC}]

    write_line [format "RAMMAP  %.9f PC=%04X PORT=%02X FULLPORT=%04X PAGE=%d SEGMENT=%02X" \
        [now] $current_pc $port $raw_port $page $value]
}

proc ::omtrace::on_io_write {} {
    variable current_pc

    set current_pc [reg PC]
    # Keep the low byte as PORT: Z80 I/O instructions address ports through
    # the complete BC register, but MoonSound decodes the low eight bits.
    set raw_port $::wp_last_address
    set port  [expr {$raw_port & 0xFF}]
    set value $::wp_last_value

    write_line [format "IO      %.9f PC=%04X PORT=%02X FULLPORT=%04X VALUE=%02X" \
        [now] $current_pc $port $raw_port $value]
}

proc ::omtrace::add_watchpoint {type address command} {
    variable ids
    set id [debug watchpoint create \
        -type $type \
        -address $address \
        -condition {} \
        -command $command]
    lappend ids [list watchpoint $id]
    return $id
}

proc ::omtrace::start {{filename ""}} {
    variable active
    variable fh
    variable ids
    variable pc_first
    variable pc_last
    variable mem_ranges
    variable io_ranges
    variable mapper_profile
    variable include_disasm
    variable trace_pc_enabled
    variable line_count
    variable default_directory

    if {$active} {
        error "Tracing is already active; run trace_stop first"
    }

    if {$filename eq ""} {
        file mkdir $default_directory
        set stamp [clock format [clock seconds] -format "%Y%m%d-%H%M%S"]
        set filename [file join $default_directory "openmsx-$stamp.log"]
    } else {
        set filename [file normalize $filename]
        file mkdir [file dirname $filename]
    }

    set fh [open $filename w]
    fconfigure $fh -buffering line -translation lf -encoding utf-8
    set ids {}
    set line_count 0

    write_line "# openMSX instruction and write trace"
    write_line "# file=$filename"
    write_line [format "# pc_range=%04X-%04X" $pc_first $pc_last]
    write_line "# memory_ranges=$mem_ranges"
    write_line "# mapper_profile=$mapper_profile"
    write_line {# columns: TYPE EMULATION_TIME PC [ADDR/PORT] [VALUE] DETAILS}

    # A true debug condition is evaluated at CPU instruction boundaries.
    # The callback only logs and does not issue 'debug break'.
    if {$trace_pc_enabled} {
        set condition [format {([reg PC] >= %d) && ([reg PC] <= %d)} \
            $pc_first $pc_last]
        set pc_id [debug condition create \
            -condition $condition \
            -command {::omtrace::on_pc}]
        lappend ids [list condition $pc_id]
    }

    foreach range $mem_ranges {
        if {[llength $range] != 2} {
            stop
            error "Each memory range must be {first last}; invalid range: $range"
        }
        lassign $range first last
        add_watchpoint write_mem [list $first $last] {::omtrace::on_mem_write}
    }

    # Optional device I/O ranges, for example MoonSound ports once confirmed.
    foreach range $io_ranges {
        if {[llength $range] != 2} {
            stop
            error "Each I/O range must be {first last}; invalid range: $range"
        }
        lassign $range first last
        add_watchpoint write_io [list $first $last] {::omtrace::on_io_write}
    }

    # Standard MSX memory mapper: page segment registers at ports FC-FF.
    add_watchpoint write_io {0xFC 0xFF} {::omtrace::on_ram_mapper_write}

    # Cartridge mapper bank-switch writes.
    foreach range [mapper_ranges $mapper_profile] {
        add_watchpoint write_mem $range {::omtrace::on_cart_mapper_write}
    }

    set active 1
    puts "openMSX trace started: $filename"
    puts "Use 'trace_stop' before exiting when possible."
    return $filename
}

proc ::omtrace::stop {} {
    variable active
    variable fh
    variable ids
    variable line_count

    # Remove callbacks before closing the file.
    foreach item $ids {
        lassign $item type id
        catch {
            if {$type eq "condition"} {
                debug condition remove $id
            } else {
                debug watchpoint remove $id
            }
        }
    }
    set ids {}

    if {$fh ne ""} {
        catch {
            puts $fh "# stopped; lines=$line_count"
            flush $fh
            close $fh
        }
        set fh ""
    }

    set was_active $active
    set active 0
    if {$was_active} {
        puts "openMSX trace stopped; lines=$line_count"
    }
}

proc ::omtrace::status {} {
    variable active
    variable pc_first
    variable pc_last
    variable mem_ranges
    variable io_ranges
    variable mapper_profile
    variable include_disasm
    variable trace_pc_enabled
    variable line_count

    set result [dict create \
        active $active \
        pc_range [list $pc_first $pc_last] \
        memory_ranges $mem_ranges \
        io_ranges $io_ranges \
        mapper_profile $mapper_profile \
        pc_enabled $trace_pc_enabled \
        disassembly $include_disasm \
        lines $line_count]
    puts $result
    return $result
}

proc ::omtrace::set_pc_range {first last} {
    variable active
    variable pc_first
    variable pc_last
    if {$active} { error "Run trace_stop before changing configuration" }
    if {$first < 0 || $first > 0xFFFF || $last < 0 || $last > 0xFFFF || $first > $last} {
        error "Invalid PC range"
    }
    set pc_first $first
    set pc_last $last
    return [list $pc_first $pc_last]
}

proc ::omtrace::set_mem_ranges {ranges} {
    variable active
    variable mem_ranges
    if {$active} { error "Run trace_stop before changing configuration" }

    foreach range $ranges {
        if {[llength $range] != 2} {
            error "Each range must be {first last}; invalid range: $range"
        }
        lassign $range first last
        if {$first < 0 || $first > 0xFFFF || $last < 0 || $last > 0xFFFF || $first > $last} {
            error "Invalid memory range: $range"
        }
    }
    set mem_ranges $ranges
    return $mem_ranges
}

proc ::omtrace::set_io_ranges {ranges} {
    variable active
    variable io_ranges
    if {$active} { error "Run trace_stop before changing configuration" }

    foreach range $ranges {
        if {[llength $range] != 2} {
            error "Each I/O range must be {first last}; invalid range: $range"
        }
        lassign $range first last
        if {$first < 0 || $first > 0xFF || $last < 0 || $last > 0xFF || $first > $last} {
            error "Invalid I/O range"
        }
    }
    set io_ranges $ranges
    return $io_ranges
}

proc ::omtrace::set_mapper {profile} {
    variable active
    variable mapper_profile
    if {$active} { error "Run trace_stop before changing configuration" }
    mapper_ranges $profile
    set mapper_profile $profile
    return $mapper_profile
}

proc ::omtrace::set_disasm {enabled} {
    variable include_disasm
    switch -nocase -- $enabled {
        on - true - 1  { set include_disasm 1 }
        off - false - 0 { set include_disasm 0 }
        default { error "Expected on or off" }
    }
    return $include_disasm
}

proc ::omtrace::set_pc_enabled {enabled} {
    variable active
    variable trace_pc_enabled
    if {$active} { error "Run trace_stop before changing configuration" }
    switch -nocase -- $enabled {
        on - true - 1  { set trace_pc_enabled 1 }
        off - false - 0 { set trace_pc_enabled 0 }
        default { error "Expected on or off" }
    }
    return $trace_pc_enabled
}

# Friendly global console commands.
proc trace_start {{filename ""}} { return [::omtrace::start $filename] }
proc trace_stop {} { ::omtrace::stop }
proc trace_status {} { return [::omtrace::status] }
proc trace_pc_range {first last} { return [::omtrace::set_pc_range $first $last] }
proc trace_mem_ranges {ranges} { return [::omtrace::set_mem_ranges $ranges] }
proc trace_io_ranges {ranges} { return [::omtrace::set_io_ranges $ranges] }
proc trace_pc_enabled {enabled} { return [::omtrace::set_pc_enabled $enabled] }
proc trace_mapper {profile} { return [::omtrace::set_mapper $profile] }
proc trace_disasm {enabled} { return [::omtrace::set_disasm $enabled] }

after quit {::omtrace::stop}

puts "Loaded openmsx_trace.tcl"
puts "Configure if needed, then run: trace_start"
puts "Recommended first: trace_pc_range 0x4000 0xBFFF"
puts "Mapper profiles: none, konami_scc, konami, ascii8, ascii16, generic"
