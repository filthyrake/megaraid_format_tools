# GitHub Actions Workflow Guide

This repository includes an automated build and release workflow using GitHub Actions.

## Workflow Overview

The `build-and-release.yml` workflow automatically:

1. **Builds** all C tools on every push to main/master and on pull requests
2. **Creates releases** when you push a version tag (e.g., `v1.0.0`)
3. **Uploads binaries** to the release as downloadable assets

## What Gets Built

The workflow compiles these tools:
- `mega_inquiry` - Drive identification tool
- `mega_format512` - FORMAT UNIT tool
- `mega_modesel` - MODE SELECT + FORMAT tool
- `check_size` - Structure validation tool

## How to Create a Release

To create a new release with pre-compiled binaries:

1. **Create and push a version tag:**
   ```bash
   git tag -a v1.0.0 -m "Release version 1.0.0"
   git push origin v1.0.0
   ```

2. **The workflow will automatically:**
   - Build all tools
   - Create a GitHub release
   - Upload compiled binaries as release assets
   - Include both individual binaries and a tarball

3. **Users can then download:**
   - Individual tool binaries
   - `megaraid-tools-linux-x86_64.tar.gz` (contains all tools, README, and LICENSE)

## Build Artifacts

On every push/PR, build artifacts are uploaded and available for 90 days in the Actions tab:
- Navigate to Actions → Select the workflow run → Download artifacts

## Requirements

The workflow uses:
- Ubuntu latest runner
- GCC compiler (via build-essential)
- Standard GitHub Actions

No additional setup or secrets are required.

## Workflow Triggers

- **Push to main/master:** Builds tools, uploads artifacts
- **Pull requests:** Builds tools to verify changes
- **Version tags (v*):** Builds tools + creates GitHub release

## Local Testing

To test the build locally before pushing:

```bash
# Install dependencies (on Debian/Ubuntu)
sudo apt-get install build-essential

# Build all tools
gcc -O2 -Wall -Wextra -o mega_inquiry mega_inquiry.c
gcc -O2 -Wall -Wextra -o mega_format512 mega_format512.c
gcc -O2 -Wall -Wextra -o mega_modesel mega_modesel.c
gcc -O2 -Wall -Wextra -o check_size check_size.c

# Verify binaries
ls -lh mega_inquiry mega_format512 mega_modesel check_size

# Create tarball
tar -czf megaraid-tools-linux-x86_64.tar.gz \
    mega_inquiry mega_format512 mega_modesel check_size \
    README.md LICENSE
```

## Troubleshooting

**Build fails:**
- Check that all `.c` files compile locally
- Verify dependencies are correctly specified in workflow

**Release not created:**
- Ensure tag follows `v*` pattern (e.g., `v1.0.0`, `v2.1.3`)
- Check that the tag was pushed to GitHub
- Verify repository has Actions enabled

**Artifacts missing:**
- Check the workflow run logs in the Actions tab
- Ensure files are created before upload step
