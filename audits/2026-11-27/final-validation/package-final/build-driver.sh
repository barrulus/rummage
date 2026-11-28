#!/usr/bin/env bash
set -euo pipefail
cat > /usr/local/bin/rpmbuild <<'WRAPPER'
#!/usr/bin/env bash
set -euo pipefail
printf 'Waiting for final Settings-fix-inclusive committed source before rpmbuild.\n'
while [[ ! -f /output/final-source-ready ]]; do sleep 1; done
exec /usr/bin/rpmbuild --noclean "$@"
WRAPPER
chmod +x /usr/local/bin/rpmbuild
cat > /usr/local/bin/ninja <<'WRAPPER'
#!/usr/bin/env bash
set -euo pipefail
args=()
for arg in "$@"; do
    if [[ "$arg" == '-j2' ]]; then args+=('-j6'); else args+=("$arg"); fi
done
exec /usr/sbin/ninja "${args[@]}"
WRAPPER
chmod +x /usr/local/bin/ninja
exec bash /checks/native-build.sh rpm
