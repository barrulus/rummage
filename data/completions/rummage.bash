# bash completion for rummage, loaded by bash-completion or sourced directly.
_rummage() {
    local cur=${COMP_WORDS[COMP_CWORD]} word value='' picker='' paths='' only_paths=''
    COMPREPLY=()
    for word in "${COMP_WORDS[@]:1:COMP_CWORD-1}"; do
        if [[ -n $value ]]; then value=''; continue; fi
        if [[ -n $only_paths ]]; then continue; fi
        case $word in
            --) only_paths=1 ;;
            --config) value=file ;;
            --title|--name) value=text ;;
            --pick|--pick-directory|--save-file) picker=1 ;;
            --reveal|--properties) paths=1 ;;
        esac
    done
    if [[ $value == file ]]; then
        mapfile -t COMPREPLY < <(compgen -f -- "$cur")
    elif [[ $value == text ]]; then
        return
    elif [[ -z $only_paths && $cur == -* ]]; then
        local options='--help --config --check-config --test-scene --version --about --reveal --properties --open-folders --portal --file-manager1 --set-file-manager --unset-file-manager --pick --pick-directory --save-file --'
        if [[ -n $picker ]]; then options+=' --multiple --name --title'; fi
        mapfile -t COMPREPLY < <(compgen -W "$options" -- "$cur")
    elif [[ -n $paths ]]; then
        mapfile -t COMPREPLY < <(compgen -f -- "$cur")
    else
        mapfile -t COMPREPLY < <(compgen -d -- "$cur")
    fi
}
complete -o filenames -F _rummage rummage
