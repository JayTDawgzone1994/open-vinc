# Poll observable state with a bound; discovery is not synchronized with sleeps.
proc await {command} {
    set deadline [expr {[clock milliseconds] + 10000}]
    while {[catch {uplevel 1 $command} diagnostic]} {
        if {[clock milliseconds] >= $deadline} { error "Recorder deadline: $command: $diagnostic" }
        after 10
    }
}
if {[catch {
    RecorderProbe
    set bag [file join [file dirname [info nameofexecutable]] recorded]
    recorder record $bag --topic sdk/recorder
    await {probe ready}
    probe burst
    await [list probe written $bag]
    recorder finish
    await [list probe complete $bag]
    recorder status
    puts "INSTALLED_RECORDER_WAIT_OK"
} diagnostic]} {
    puts "INSTALLED_RECORDER_FAILED: $diagnostic"
}
exit
