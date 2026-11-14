# ccos: the Terminal's shell (bash)
[ -z "$PS1" ] && return
# a coloured prompt:  ccos ~/folder $   (red # instead of $ would mean "root": ccos runs everything as root)
PS1='\[\e[1;36m\]ccos\[\e[0m\] \[\e[1;34m\]\w\[\e[0m\] \[\e[1;32m\]\$\[\e[0m\] '
PROMPT_DIRTRIM=3
# history: kept on the stick, shared between Terminals, Ctrl+R searches it
if mountpoint -q /data 2>/dev/null; then HISTFILE=/data/.bash_history; fi
HISTSIZE=5000; HISTFILESIZE=20000; HISTCONTROL=ignoreboth:erasedups
shopt -s histappend checkwinsize cdspell autocd globstar 2>/dev/null
PROMPT_COMMAND='history -a'
# colours
alias ls='ls --color=auto' ll='ls -lh --color=auto' la='ls -lah --color=auto' l='ls -CF --color=auto'
alias grep='grep --color=auto'
alias ..='cd ..' ...='cd ../..'
alias py=python3 python=python3 pip='python3 -m pip' pip3='python3 -m pip'
alias D:='cd /data' d:='cd /data'
export LS_COLORS='di=1;34:ln=1;36:ex=1;32:*.py=0;33:*.zip=1;31:*.7z=1;31:*.rar=1;31:*.gz=1;31:*.jpg=1;35:*.png=1;35:*.mp4=1;35:*.mp3=0;36:*.pdf=0;31'
# Tab completion for commands' options (and folders, files...)
[ -f /usr/share/bash-completion/bash_completion ] && . /usr/share/bash-completion/bash_completion

help() {
    if [ -n "$1" ]; then builtin help "$@" 2>/dev/null || man "$1" 2>/dev/null || "$1" --help 2>&1 | less -FX; return; fi
    local c=$'\e[1;36m' b=$'\e[1m' n=$'\e[0m'
    cat <<HELP
${c}ccos Terminal${n} - useful commands   (help COMMAND tells more about one)

${b}Files${n}
  ls / ll           list files (ll: with sizes and dates)
  cd FOLDER         go into a folder (cd .. goes up, D: goes to your D: drive)
  cp A B / mv A B   copy / move or rename          rm FILE   delete (no Recycle Bin!)
  mkdir NAME        make a folder                  tree      folders as a tree
  nano FILE         edit a text file (Ctrl+S saves, Ctrl+X quits)
  cat / less FILE   show / read a file (q quits)   find . -name '*.txt'   find files
  zip -r a.zip DIR  zip a folder    unzip a.zip    7z x FILE   unpack .7z .rar ...

${b}System${n}
  htop              running programs, CPU, memory (F9 ends one, q quits)
  sensors           temperatures and fans          df -h     free space
  free -h           memory          lsusb / lspci  devices   poweroff / reboot

${b}Internet${n}
  ping google.com   is the internet working?       ip a      network addresses
  curl URL          download (curl -O URL saves it; curl wttr.in = the weather)
  ssh user@host     log in to another computer (scp copies files)

${b}Python${n}
  python3           Python (exit() leaves)         python3 FILE.py   run a program
  pip install NAME  add a package (kept on your stick), or use the Python app

${b}Keys${n}
  Tab               finish a name (Tab Tab: the choices)
  Up / Down         earlier commands (type the start first to narrow them)
  Ctrl+R            search earlier commands        Ctrl+L    clear the screen
  Ctrl+C            stop the running program       Ctrl+Shift+C / V   copy / paste
HELP
}

# the welcome text, in a new Terminal
shopt -q login_shell && [ -f /etc/motd ] && cat /etc/motd
