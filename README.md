# halen 
![halen-shotcr](https://github.com/user-attachments/assets/14e38e81-1df2-4e4b-8286-c16f40e10910)

Clipboard manager inspired by [Clip**Jump**](https://clipjump.sourceforge.net/help.htm) .
UI is a popup notification showing one clipboard history entry at a time.
Navigate the history by holding `Ctrl` and press `V` or `C`.
When `Ctrl` is released, the current entry will be copied to the clipboard,
and a *fake* `Ctrl+V` will be sent and the notification closed.
The notification is spawned by holding `Ctrl` and pressing `V` twice. (**Ctrl+V+V**).

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

# dependencies
X11, **xclip**
