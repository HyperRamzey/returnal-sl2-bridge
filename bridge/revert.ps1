# revert.ps1 - restore the pristine Streamline 1.3.3 installation
& (Join-Path $PSScriptRoot 'install.ps1') -Remove
