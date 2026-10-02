# Run with srun, then press Ctrl+D. Ctrl+C shuts down the whole framework.
set count 0
while {1} {
    puts "Script tick [incr count] (Ctrl+D to cancel)"
    after 1000
}
