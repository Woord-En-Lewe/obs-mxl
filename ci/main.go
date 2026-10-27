// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 obs-mxl contributors

// Dagger CI for obs-mxl. All functions build inside Ubuntu 24.04 with OBS 30
// headers; MXL 1.1.0 is built from source with vcpkg and cached in Dagger
// volumes.
package main

import (
	"context"
	"fmt"
	"strings"

	"dagger/obs-mxl/internal/dagger"
)

type ObsMxl struct{}

const (
	obsImage   = "ubuntu:24.04"
	mxlVersion = "v1.1.0"
	mxlPrefix  = "/opt/mxl"
)

var aptPackages = []string{
	"bison",
	"build-essential",
	"ca-certificates",
	"clang-format",
	"cmake",
	"curl",
	"flex",
	"git",
	"libgl1-mesa-dev",
	"libobs-dev",
	"m4",
	"ninja-build",
	"obs-studio",
	"pkg-config",
	"qt6-base-dev",
	"qt6-base-dev-tools",
	"libqt6svg6-dev",
	"tar",
	"unzip",
	"zip",
}

// base returns the Ubuntu image with the OBS, Qt and build dependencies.
func (m *ObsMxl) base() *dagger.Container {
	args := append([]string{"apt-get", "install", "-y", "--no-install-recommends"}, aptPackages...)
	return dag.Container().
		From(obsImage).
		WithEnvVariable("DEBIAN_FRONTEND", "noninteractive").
		WithExec([]string{"apt-get", "update"}).
		WithExec(args).
		WithExec([]string{"bash", "-lc", "rm -rf /var/lib/apt/lists/*"})
}

// mxlSdk builds and installs libmxl into /opt/mxl. vcpkg binary archives are
// cached across runs.
func (m *ObsMxl) mxlSdk() *dagger.Container {
	return m.base().
		WithMountedCache("/root/.cache/vcpkg/archives", dag.CacheVolume("obs-mxl-vcpkg-archives")).
		WithExec([]string{"git", "clone", "https://github.com/microsoft/vcpkg", "/opt/vcpkg"}).
		WithExec([]string{"/opt/vcpkg/bootstrap-vcpkg.sh", "-disableMetrics"}).
		WithExec([]string{
			"git", "clone", "--depth", "1", "--branch", mxlVersion,
			"https://github.com/dmf-mxl/mxl", "/src/mxl",
		}).
		WithExec([]string{
			"cmake", "-S", "/src/mxl", "-B", "/src/mxl/build", "-G", "Ninja",
			"-DCMAKE_BUILD_TYPE=Release",
			"-DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake",
			"-DBUILD_TESTS=OFF",
			"-DBUILD_TOOLS=OFF",
			"-DBUILD_UTILS=OFF",
			"-DBUILD_DOCS=OFF",
			"-DMXL_ENABLE_FABRICS_OFI=OFF",
			"-DCMAKE_INSTALL_PREFIX=" + mxlPrefix,
		}).
		WithExec([]string{"cmake", "--build", "/src/mxl/build", "-j", "4"}).
		WithExec([]string{"cmake", "--install", "/src/mxl/build"}).
		WithEnvVariable("CMAKE_PREFIX_PATH", mxlPrefix)
}

// sourceContainer mounts the repository into the MXL build environment.
func (m *ObsMxl) sourceContainer(source *dagger.Directory) *dagger.Container {
	return m.mxlSdk().
		WithDirectory("/src/obs-mxl", source, dagger.ContainerWithDirectoryOpts{
			Exclude: []string{".git", ".mxl-sdk", "build", "build-*", "ci/internal"},
		}).
		WithWorkdir("/src/obs-mxl")
}

func configureArgs(buildType string) []string {
	return []string{
		"cmake", "-S", ".", "-B", "build", "-G", "Ninja",
		"-DCMAKE_BUILD_TYPE=" + buildType,
		"-DOBS_MXL_ENABLE_PLUGIN=ON",
		"-DOBS_MXL_ENABLE_UI=ON",
		"-DOBS_MXL_BUILD_TESTS=ON",
		"-DOBS_MXL_WARNINGS_AS_ERRORS=ON",
	}
}

// Build compiles the OBS module and returns the shared object.
func (m *ObsMxl) Build(ctx context.Context, source *dagger.Directory) (*dagger.File, error) {
	return m.sourceContainer(source).
		WithExec(configureArgs("Release")).
		WithExec([]string{"cmake", "--build", "build", "--target", "obs-mxl", "-j", "4"}).
		File("/src/obs-mxl/build/obs-mxl.so"), nil
}

// Test builds everything and runs the complete CTest suite, including the
// libmxl integration tests.
func (m *ObsMxl) Test(ctx context.Context,
	// +defaultPath="."
	source *dagger.Directory) (string, error) {
	return m.sourceContainer(source).
		WithExec(configureArgs("RelWithDebInfo")).
		WithExec([]string{"cmake", "--build", "build", "-j", "4"}).
		WithExec([]string{"ctest", "--test-dir", "build", "--output-on-failure"}).
		Stdout(ctx)
}

// Lint verifies that all C++ sources are clang-format clean.
func (m *ObsMxl) Lint(ctx context.Context,
	// +defaultPath="."
	source *dagger.Directory) (string, error) {
	script := strings.Join([]string{
		"set -euo pipefail",
		"find include src tests -type f \\( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \\) -print0",
		"| xargs -0 clang-format --dry-run --Werror",
	}, " ")
	return m.base().
		WithDirectory("/src/obs-mxl", source, dagger.ContainerWithDirectoryOpts{
			Exclude: []string{".git", ".mxl-sdk", "build", "build-*", "ci/internal"},
		}).
		WithWorkdir("/src/obs-mxl").
		WithExec([]string{"bash", "-lc", script}).
		Stdout(ctx)
}

// Package returns a relocatable tarball with the plugin, locale and docs.
func (m *ObsMxl) Package(ctx context.Context, source *dagger.Directory) (*dagger.File, error) {
	build, err := m.Build(ctx, source)
	if err != nil {
		return nil, err
	}

	return dag.Container().
		From("alpine:3.20").
		WithDirectory("/package", dag.Directory().
			WithFile("obs-mxl.so", build).
			WithDirectory("locale", source.Directory("data/locale")).
			WithFile("README.md", source.File("README.md")).
			WithFile("LICENSE", source.File("LICENSE"))).
		WithWorkdir("/package").
		WithExec([]string{"tar", "-czf", "/obs-mxl.tar.gz", "."}).
		File("/obs-mxl.tar.gz"), nil
}

// Ci runs the full pipeline: formatting, build, tests.
func (m *ObsMxl) Ci(ctx context.Context,
	// +defaultPath="."
	source *dagger.Directory) (string, error) {
	lint, err := m.Lint(ctx, source)
	if err != nil {
		return "", fmt.Errorf("lint failed: %w", err)
	}
	_ = lint

	test, err := m.Test(ctx, source)
	if err != nil {
		return "", fmt.Errorf("tests failed: %w", err)
	}

	return test, nil
}
