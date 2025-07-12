# halen 
![halen-shotcr](https://github.com/user-attachments/assets/14e38e81-1df2-4e4b-8286-c16f40e10910)

Clipboard manager inspired by [Clip**Jump**](https://clipjump.sourceforge.net/help.htm) .
UI is a popup notification showing one clipboard history entry at a time.
Navigate the history by holding `Ctrl` and press `V` or `C`.
When `Ctrl` is released, the current entry will be copied to the clipboard,
a *fake* `Ctrl+V` will be sent and the notification closed.
The notification is spawned by holding `Ctrl` and pressing `V` twice. (**Ctrl+V+V**).  

I put a demonstration of the program on [YouTube](https://www.youtube.com/watch?v=l_9PLQGuNys).

# installation

```
$ make
# make install
```

# dependencies

**Build dependencies (Arch Linux):**
```
pacman -S pkg-config libx11 libxtst libxext libxfixes fontconfig libxft
```
Also a C compiler and **GNU**/Make is needed.

**Runtime dependencies:**
- `xclip`

# usage

**~/.config/halen/config**  
```
font = monospace
font_size = 16
background = #FCFFDF
foreground = #000000
count_color = #3322DD
anchor = 5
position = mouse
margin = 30 10
max_line_length = 80
max_lines = 10
```

**Commandline options:**  
```
  -V, --verbose         Enable verbose (debug) logging
  -c, --config FILE     Use configuration file
  -t, --toggle          Toggle keyboard grabs
  -h, --help            Show this help message
      --version         Show version information
```

---

A history file will get created in **XDG_CACHE_HOME**/halen/history , this is also
where cached versions of clips that exceeds the line limits will be stored.  

A PID file will get created at **XDG_RUNTIME_DIR/halen.pid** it contains the PID of the currently running halen process.

>Note that `halen --toggle` can be executed while the program is running, it will use
the pid in the pidfile to send **USR1** signal which in turn toggle keyboard grabs.


# known issues

Program was developed and tested on Arch Linux with a simple X setup with i3wm,
it should work on any Linux with X11, but i don't know. Also pretty sure it will
not work without modifications on other UNIX based systems.
