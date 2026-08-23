# dumsh web-shell zshrc
export ZSH="$HOME/.oh-my-zsh"
export PATH="$HOME/.local/bin:$PATH"
export TERM="xterm-256color"

if [ -f "$ZSH/oh-my-zsh.sh" ]; then
  source "$ZSH/oh-my-zsh.sh"
fi

plugins=(git gitfast zsh-interactive-cd colored-man-pages)

alias ll='ls -al'
alias la='ls -A'
alias l='ls -CF'

PROMPT='usr40k@dumsh:%~$ '
