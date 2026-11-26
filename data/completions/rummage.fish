# fish completion for rummage.
function __rummage_context
    set -l value 0
    set -l mode ''
    set -l only_paths 0
    for word in (commandline -opc)[2..-1]
        if test $value = 1
            set value 0
            continue
        end
        if test $only_paths = 1
            continue
        end
        switch $word
            case --
                set only_paths 1
            case --config --title --name
                set value 1
            case --pick --pick-directory --save-file --reveal --properties --open-folders --portal --file-manager1 --set-file-manager --unset-file-manager --test-scene
                set mode $word
        end
    end
    switch $argv[1]
        case modes
            test $only_paths = 0; and test -z "$mode"
        case window
            test $only_paths = 0; and contains -- "$mode" '' --open-folders
        case picker
            test $only_paths = 0; and contains -- "$mode" --pick --pick-directory --save-file
        case files
            contains -- "$mode" --reveal --properties
        case directories
            not contains -- "$mode" --reveal --properties
    end
end
complete -c rummage -n '__rummage_context window' -l new-window -d 'Open a separate browser window'
complete -c rummage -f
complete -c rummage -n '__rummage_context directories' -a '(__fish_complete_directories)'
complete -c rummage -n '__rummage_context files' -F
complete -c rummage -s h -l help -d 'Show usage'
complete -c rummage -l config -r -F -d 'Read configuration from a file'
complete -c rummage -l check-config -d 'Check configuration and exit'
complete -c rummage -n '__rummage_context modes' -l test-scene -d 'Show the renderer test scene'
complete -c rummage -l version -d 'Print the version'
complete -c rummage -l about -d 'Print diagnostic information'
complete -c rummage -n '__rummage_context modes' -l reveal -d 'Select paths in their folders'
complete -c rummage -n '__rummage_context modes' -l properties -d 'Show properties of paths'
complete -c rummage -n '__rummage_context modes' -l open-folders -d 'Open folders in tabs'
complete -c rummage -n '__rummage_context modes' -l portal -d 'Run the file chooser portal backend'
complete -c rummage -n '__rummage_context modes' -l file-manager1 -d 'Run the Show in Folder service'
complete -c rummage -n '__rummage_context modes' -l set-file-manager -d 'Register as the file manager'
complete -c rummage -n '__rummage_context modes' -l unset-file-manager -d 'Remove file manager registration'
complete -c rummage -n '__rummage_context modes' -l pick -d 'Choose files'
complete -c rummage -n '__rummage_context modes' -l pick-directory -d 'Choose a folder'
complete -c rummage -n '__rummage_context modes' -l save-file -d 'Choose a file to save'
set -l picker '__rummage_context picker'
complete -c rummage -n $picker -l multiple -d 'Choose multiple files'
complete -c rummage -n $picker -l name -x -d 'Proposed filename'
complete -c rummage -n $picker -l title -x -d 'Window title'
