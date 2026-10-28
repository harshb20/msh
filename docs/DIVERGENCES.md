# Intentional Bash divergences

`msh` implements the project scope, not the entire Bash language. The 134-case differential suite is 100% green within that scope. The following differences are explicit and tested or rejected rather than silently approximated.

| Bash feature | `msh` behavior | Reason left out |
|---|---|---|
| Command substitution: `$(...)`, backticks | Treated as ordinary characters/tokens; not executed | Requires nested parsing plus output capture and is outside the requested expansion scope |
| Arithmetic expansion and `(( ... ))` | Not implemented | Separate expression language |
| Brace expansion and arrays | Not implemented | Bash-specific language surface, unrelated to process/fd goals |
| Shell functions and sourced files | Not implemented | Would require scopes, positional parameters, and a larger runtime |
| Subshell/group syntax: `( ... )`, `{ ...; }` | Not implemented | Compound-command AST and subshell semantics are outside scope |
| Here-documents, here-strings, `|&` | Rejected/not interpreted as Bash | The requested redirection forms are implemented; these require extra grammar and buffering |
| Arbitrary fd redirections such as `7>file` | Only stdin/stdout/stderr forms in the feature table are recognized | Keeps descriptor syntax bounded to the brief |
| `NAME=value command` assignment words | Use `export NAME=value` instead | Avoids adding temporary command environments and assignment-word parsing |
| Advanced parameter forms such as `${x:-default}` | Reports a bad substitution | Only `$VAR`, `${VAR}`, `$?`, and `$$` are in scope |
| Custom `IFS` field splitting | Uses shell-default space/tab/newline splitting | Full POSIX `IFS` edge semantics are large and not requested |
| Background `&&`/`||` compound lists | Reports an unsupported-compound error with status 2 | Correct Bash behavior requires an asynchronous subshell for the whole list; simple and pipelined `&` jobs are fully supported |
| Advanced job specs (`%-`, `%?text`, `%prefix`) | Supports current job, `%+`, `%%`, `%N`, and `N` | Sufficient for `jobs`/`fg`/`bg` demonstrations without a job-spec mini-language |
| `export` with no arguments | Returns an explanatory error instead of printing the environment | Listing/quoting every exported variable is not part of state mutation |
| Persistent history file | History lasts for the current shell session | Avoids unrequested filesystem policy and locking |
| Full Unicode display-width editing | Input bytes are preserved, but cursor columns are byte-oriented | Correct grapheme and East Asian width handling needs a Unicode width library |
| Interactive completion/search | Not implemented | The raw editor focuses on history and cursor keys required by the project |

These boundaries make useful interview material: each omitted feature has a concrete grammar, state, or process-model cost rather than being an accidental failure.
