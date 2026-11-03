if {[catch {
    puts "CENTRAL_PID=[pid]"
    if {[NativePeer [info nameofexecutable]] != 0} { error "Native startup failed" }
    if {[PythonPeer --name py @REVISION@] != 0} { error "Python startup failed" }
    py send
    set deadline [expr {[clock milliseconds] + 8000}]
    while {[catch {py check}]} {
        if {[clock milliseconds] > $deadline} { error "Python communication timed out" }
        after 20
    }
    native status
    param get native.fixed
    param get py.gain
    param list
    # Same-value commits still report declared metadata, without restarting peers.
    param set native.fixed 7
    param set py.gain 8
    puts "INSTALLED_PYTHON_OK"
} diagnostic]} {
    puts "INSTALLED_PYTHON_FAILED: $diagnostic"
}
exit
