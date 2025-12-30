### Updating C++ WinRT headers (winrt/ subdirectory)

These headers are imported from the official Windows SDK. A GitHub Action checks
that files are exact copies of Windows SDK headers. When updating headers files,
please update the corresponding SDK version in [.github\workflows\check-cpp-winrt-headers.sh]().
