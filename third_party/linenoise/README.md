# linenoise patch

The shell edits lines on the terminal at standard input and writes prompts to standard error,
since standard output carries rows. `terminal_fds.patch` makes linenoise 2.0 keep to those file
descriptors and keep input typed ahead:

- `linenoiseEditStop` writes its newline, and Ctrl+L its escape sequence, to the output file
  descriptor rather than to standard output.
- The terminal width comes from the input file descriptor rather than from standard output, which
  may be redirected.
- Raw mode is entered and left with `TCSADRAIN` rather than `TCSAFLUSH`, which discards the rest
  of a multi-line paste after the first line.
