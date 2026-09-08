# Releases

Pushes and pull requests run repository checks and a Windows SDK build.
They do not publish archives or releases.

The platform release workflows run only for an explicitly created `v*` tag.
Creating or pushing a release tag requires owner approval. The tag must identify
the exact SDK revision intended for downstream Vana360 consumers.

Approved release tags publish builds in the fork's
[GitHub Releases](https://github.com/REVana360/vana360-sdk/releases).
The upstream project publishes its own builds and release notes at
[rexglue/rexglue-sdk releases](https://github.com/rexglue/rexglue-sdk/releases).

The release workflows fetch upstream `v*` tags under the local
`upstream/v*` reference namespace. Version resolution always prefers a
reachable fork `v*` tag and uses an upstream tag only as a fallback baseline,
so upstream tags cannot replace a fork release tag with the same version.
