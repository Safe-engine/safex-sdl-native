#!/usr/bin/env sh

if [ -n "${SAFEX_ROOT:-}" ]; then
    echo "Your path is: ${SAFEX_ROOT}"
    exit 0
fi

if [ "${SHELL:-}" = "/bin/zsh" ]; then
    profile="$HOME/.zshrc"
else
    profile="$HOME/.bash_profile"
fi

setting="export SAFEX_ROOT=\"$(pwd)\""
if ! grep -Fqx "$setting" "$profile" 2>/dev/null; then
    printf '\n%s\n' "$setting" >> "$profile"
fi

echo "Added SAFEX_ROOT to $profile. Open a new terminal or run: . $profile"
