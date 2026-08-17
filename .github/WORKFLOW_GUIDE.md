# GitHub Actions Workflow Guide

This repository includes an automated build and release workflow using GitHub Actions.

## Workflow Overview

The `build-and-release.yml` workflow automatically:

1. **Builds** all C tools on every push to main/master and on pull requests
2. **Runs the sense-parser tests, a no-args smoke test, and an ioctl struct-divergence check**
3. **Creates releases** when you push a version tag (e.g., `v1.0.0`)
4. **Uploads binaries** to the release as downloadable assets

## What Gets Built

The workflow compiles these tools:
- `mega_inquiry` - Drive identification tool
- `mega_format512` - FORMAT UNIT tool
- `mega_modesel` - MODE SELECT + FORMAT tool
- `mega_format_immed` - MODE SELECT + FORMAT UNIT with IMMED (background format)
- `mega_progress` - Background FORMAT UNIT progress poller
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

- **Push to main/master:** Builds and tests tools, uploads artifacts
- **Pull requests:** Builds and tests tools to verify changes
- **Version tags (v*):** Builds and tests tools + creates GitHub release

## Local Testing

To test the build locally before pushing:

```bash
# Install dependencies (on Debian/Ubuntu)
sudo apt-get install build-essential

# Build all tools
gcc -O2 -Wall -Wextra -o mega_inquiry mega_inquiry.c
gcc -O2 -Wall -Wextra -o mega_format512 mega_format512.c
gcc -O2 -Wall -Wextra -o mega_modesel mega_modesel.c
gcc -O2 -Wall -Wextra -o mega_format_immed mega_format_immed.c
gcc -O2 -Wall -Wextra -o mega_progress mega_progress.c
gcc -O2 -Wall -Wextra -o check_size check_size.c

# Verify binaries
ls -lh mega_inquiry mega_format512 mega_modesel mega_format_immed mega_progress check_size

# Run the sense-parser tests (CI runs these too)
sh run_tests.sh

# Smoke test: every tool must exit 1 and print usage with no arguments
for t in mega_inquiry mega_format512 mega_modesel mega_format_immed mega_progress; do
    ./"$t" >/dev/null 2>&1 && { echo "FAIL: $t exited 0"; break; }
done

# Create tarball
tar -czf megaraid-tools-linux-x86_64.tar.gz \
    mega_inquiry mega_format512 mega_modesel mega_format_immed mega_progress check_size \
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
