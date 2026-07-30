# Bash completion for nash
# Source this file: . nash-completion.bash
# Or copy to /etc/bash_completion.d/nash
# Or symlink to ~/.local/share/bash-completion/completions/nash

_nash_complete() {
    local cur prev opts nash_dir
    COMPREPLY=()
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"

    # Resolve data directory (check --data-dir in current cmdline, else default)
    nash_dir="$HOME/.nash"
    local i
    for ((i=1; i<COMP_CWORD; i++)); do
        if [[ "${COMP_WORDS[i]}" == "--data-dir" && $((i+1)) -lt $COMP_CWORD ]]; then
            nash_dir="${COMP_WORDS[$((i+1))]}"
        fi
    done

    case "$prev" in
        -w|--workspace)
            _nash_complete_workspace "$nash_dir" "$cur"
            return 0
            ;;
        --provider)
            _nash_complete_provider "$nash_dir" "$cur"
            return 0
            ;;
        --play)
            _nash_complete_playbook "$nash_dir" "$cur"
            return 0
            ;;
        --optimize)
            COMPREPLY=($(compgen -W "light medium heavy" -- "$cur"))
            return 0
            ;;
        --split)
            COMPREPLY=($(compgen -W "held-in held-out" -- "$cur"))
            return 0
            ;;
        --validate-harness)
            COMPREPLY=($(compgen -W "baseline compare" -- "$cur"))
            return 0
            ;;
        --validate-playbook|--load-spec)
            COMPREPLY=($(compgen -f -- "$cur"))
            return 0
            ;;
        --data-dir|--session)
            COMPREPLY=($(compgen -d -- "$cur"))
            return 0
            ;;
        --api|--reflect-model|-p|--query)
            # No useful completion for URLs, model names, or query text
            return 0
            ;;
        --postmortem-sessions|--epochs|--edit-budget|--mailbox-timeout)
            # Numeric arguments - no completion
            return 0
            ;;
        --agent)
            # After --agent, complete with agent IDs or boolean flags
            _nash_complete_agent "$nash_dir" "$cur"
            return 0
            ;;
    esac

    # If current word starts with -, complete options
    if [[ "$cur" == -* ]]; then
        opts="
            --api
            --provider
            -p --query
            --play
            --data-dir
            --session
            --regression
            --split
            --validate-harness
            --validate-playbook
            --postmortem
            --postmortem-sessions
            --optimize
            --reflect-model
            --epochs
            --edit-budget
            --spec
            --load-spec
            -w --workspace
            -wl --workspace-list
            --isolated
            --mailbox
            --daemon
            --telegram
            --matrix
            --mailbox-timeout
            --agent
            --dry-run
            --due
            --help
        "
        COMPREPLY=($(compgen -W "$opts" -- "$cur"))
        return 0
    fi

    # Positional: directory completion (PATH for repo map context)
    COMPREPLY=($(compgen -d -- "$cur"))
    return 0
}

_nash_complete_workspace() {
    # Complete workspace names, supporting nested paths (e.g. rh/container-tools)
    # $1 = nash_dir, $2 = current word
    local nash_dir="$1" cur="$2"
    local ws_dir="$nash_dir/workspaces"

    [[ -d "$ws_dir" ]] || return

    local completions=()
    local prefix="" search_dir=""

    if [[ "$cur" == */* ]]; then
        # User already typed a prefix with slash - complete within that subdir
        prefix="${cur%/*}/"
        search_dir="$ws_dir/${cur%/*}"
    else
        prefix=""
        search_dir="$ws_dir"
    fi

    [[ -d "$search_dir" ]] || return

    local d name
    for d in "$search_dir"/*/; do
        [[ -d "$d" ]] || continue
        name=$(basename "$d")
        [[ "$name" == .* ]] && continue
        # Skip internal dirs (memory, sessions, agent) - these are not workspaces
        case "$name" in
            memory|sessions|agent) continue ;;
        esac
        # Check if this dir has sub-workspaces (contains dirs that are NOT internal)
        local has_sub=0
        local sub
        for sub in "$d"/*/; do
            [[ -d "$sub" ]] || continue
            local subname
            subname=$(basename "$sub")
            case "$subname" in
                memory|sessions|agent) ;;
                *) has_sub=1; break ;;
            esac
        done
        if [[ $has_sub -eq 1 ]]; then
            # This is a namespace with sub-workspaces - add with trailing /
            completions+=("${prefix}${name}/")
        else
            # This is a leaf workspace
            completions+=("${prefix}${name}")
        fi
    done

    COMPREPLY=($(compgen -W "${completions[*]}" -- "$cur"))

    # Don't add space after completions ending with / (allow further completion)
    if [[ ${#COMPREPLY[@]} -eq 1 && "${COMPREPLY[0]}" == */ ]]; then
        compopt -o nospace
    fi
}

_nash_complete_provider() {
    # Complete provider names from [providers.NAME] sections in config.toml
    # $1 = nash_dir, $2 = current word
    local nash_dir="$1" cur="$2"
    local config="$nash_dir/config.toml"

    [[ -f "$config" ]] || return

    local providers
    providers=$(sed -n 's/^\[providers\.\(.*\)\]/\1/p' "$config" 2>/dev/null)
    COMPREPLY=($(compgen -W "$providers" -- "$cur"))
}

_nash_complete_playbook() {
    # Complete playbook names from ~/.nash/playbooks/*.yaml
    # $1 = nash_dir, $2 = current word
    local nash_dir="$1" cur="$2"
    local pb_dir="$nash_dir/playbooks"

    [[ -d "$pb_dir" ]] || return

    local names=""
    local f name
    for f in "$pb_dir"/*.yaml "$pb_dir"/*.yml; do
        [[ -f "$f" ]] || continue
        name=$(basename "$f")
        name="${name%.yaml}"
        name="${name%.yml}"
        names="$names $name"
    done
    COMPREPLY=($(compgen -W "$names" -- "$cur"))
}

_nash_complete_agent() {
    # Complete agent IDs (workspace names that have agent.toml or agent/ dir)
    # Also complete --due and --dry-run as sub-flags
    # $1 = nash_dir, $2 = current word
    local nash_dir="$1" cur="$2"

    if [[ "$cur" == -* ]]; then
        COMPREPLY=($(compgen -W "--due --dry-run" -- "$cur"))
        return
    fi

    local ws_dir="$nash_dir/workspaces"
    [[ -d "$ws_dir" ]] || return

    local agents=""
    local d
    # Scan for agent.toml files in workspace dirs
    while IFS= read -r -d '' d; do
        local rel="${d#$ws_dir/}"
        rel="${rel%/agent.toml}"
        agents="$agents $rel"
    done < <(find "$ws_dir" -name 'agent.toml' -print0 2>/dev/null)

    # Also check for agent/ directories
    while IFS= read -r -d '' d; do
        local rel="${d#$ws_dir/}"
        rel="${rel%/agent}"
        # Avoid duplicates
        case " $agents " in
            *" $rel "*) ;;
            *) agents="$agents $rel" ;;
        esac
    done < <(find "$ws_dir" -mindepth 2 -maxdepth 3 -type d -name 'agent' -print0 2>/dev/null)

    COMPREPLY=($(compgen -W "$agents" -- "$cur"))
}

complete -F _nash_complete nash
