.DEFAULT_GOAL := help

.PHONY: help build flash release local-release

help:
	@printf '%s\n' \
	  'make build                         Build the firmware locally.' \
	  'make flash PORT=/dev/ttyUSB0        Flash firmware without opening a monitor.' \
	  'make release VERSION=0.2.1         Publish a release built by GitHub Actions.' \
	  'make local-release VERSION=0.2.1   Build and publish a release from this machine.'

build:
	./scripts/build.sh

flash:
	@set -eu; \
	if [ -z '$(PORT)' ]; then \
		printf '%s\n' 'Usage: make flash PORT=/dev/ttyUSB0' >&2; exit 2; \
	fi; \
	./scripts/flash.sh '$(PORT)'

# The GitHub workflow triggered by the tag builds the OTA artifact, manifest,
# checksum, and GitHub Release.  Deliberately do not stage or commit changes:
# a release must always identify an explicit, reviewed commit.
release:
	@set -eu; \
	version='$(VERSION)'; \
	if ! printf '%s\n' "$$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$$'; then \
		printf '%s\n' 'Usage: make release VERSION=MAJOR.MINOR.PATCH' >&2; exit 2; \
	fi; \
	if ! git diff --quiet || ! git diff --cached --quiet; then \
		printf '%s\n' 'Working tree is not clean; commit or stash changes first.' >&2; exit 1; \
	fi; \
	tag="v$$version"; \
	if git rev-parse -q --verify "refs/tags/$$tag" >/dev/null; then \
		printf 'Tag %s already exists.\n' "$$tag" >&2; exit 1; \
	fi; \
	git push origin HEAD:main; \
	git tag -a "$$tag" -m "$$tag"; \
	git push origin "$$tag"; \
	printf 'Published %s. Follow the Release firmware workflow in GitHub Actions.\n' "$$tag"

local-release:
	bash ./scripts/local-release.sh '$(VERSION)'
