# Release notes

One Markdown file per release, named after its git tag: `v3.8.md` for tag `v3.8`.

Pushing a tag that starts with `v` runs `.github/workflows/release.yml`: it runs the CI, builds
`TallyCCUPro-<tag>.zip` (firmware, web pages with their `.gz` copies, tools, README and
license) and `TallyCCU_Configurator.exe`, and publishes the GitHub release with
`docs/release-notes/<tag>.md` as its description. The workflow stops if that file is missing,
so commit the notes before pushing the tag:

```bash
git add docs/release-notes/v3.9.md && git commit -m "Release notes v3.9"
git tag v3.9 && git push origin main v3.9
```

A tag with a hyphen (`v3.9-rc1`) is published as a pre-release. The full history of changes is
also in the Changelog section of the main README.
