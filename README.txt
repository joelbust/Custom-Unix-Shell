# COP5570 Project 2 - FSU Shell (fsh)

## 1) Build and run

- Build:
  - `make`
- Run:
  - `./fsh`
- Run demo target:
  - `make demo`
- Clean:
  - `make clean`

Build flags used: `-Wall -std=c11 -pedantic`.

## 2) Demo reproduction plan

Use one terminal session and demonstrate the following sequence:

1. Basic execution:
   - `/bin/echo hello`
   - `/bin/ls -l`
2. `FSH_PATH` search:
   - `set FSH_PATH = /usr/bin+/bin`
   - `which ls`
   - `whereis ls`
   - `show`
3. Prompt variables:
   - `set prompt = <$host:$dir:$c> `
4. History and repeat:
   - `history 5`
   - `!wh` (or another existing prefix)
5. Multi-command line:
   - `pwd; date; whoami`
6. Redirection:
   - `echo abc > out.txt`
   - `cat < out.txt`
7. Pipes (up to 5):
   - `cat fsh.c | grep main | wc -l`
8. Background/foreground:
   - `sleep 20 &`
   - `sleep 20` then press `Ctrl-C` (kills only foreground)
9. Line editing keys:
   - Up/Down arrows or `Ctrl-P`/`Ctrl-N`
   - Backspace and `Ctrl-A`
10. Logging:
   - `log on`
   - run a couple commands
   - `log off`
11. Launch and restart:
   - `launch sleep 3`
   - observe relaunch behavior
12. `Ctrl-K`:
   - press `Ctrl-K` to kill all child process groups created by this shell
13. Exit:
   - `exit`

## 3) AI-tool usage

AI assistance assistence was used for outline and some problem solving.