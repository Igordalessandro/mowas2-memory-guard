# Release checksums

Each `SHA256SUMS-<version>.txt` records the SHA-256 of that release's
`dbghelp.dll`. Manifests are byte-for-byte unchanged by the directory cleanup.
They are also attached to their corresponding GitHub releases using the same
filenames.

To check a DLL, run `sha256sum -c` from the directory containing that DLL,
passing the path to the matching manifest. For the local experimental.15 build,
from the repository root:

```sh
(cd dist/v2.5.0-experimental.15 && sha256sum -c ../../checksums/SHA256SUMS-v2.5.0-experimental.15.txt)
```

On Windows, compare `Get-FileHash .\dbghelp.dll -Algorithm SHA256` with the
hash in the corresponding manifest. A checksum confirms the file matches the
release; it is not a guarantee of gameplay or account safety.
