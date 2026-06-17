# Generate a random SIGN_KEY for GitHub Actions + device config.h
$bytes = New-Object byte[] 32
[System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
$key = [Convert]::ToBase64String($bytes)

Write-Host ""
Write-Host "Generated SIGN_KEY (save this somewhere safe):" -ForegroundColor Green
Write-Host $key
Write-Host ""
Write-Host "Next steps:"
Write-Host "  1. GitHub -> Settings -> Secrets -> Actions -> New secret"
Write-Host "     Name: SIGN_KEY"
Write-Host "     Value: (paste the key above)"
Write-Host "  2. Paste the same value into include/config.h as SIGN_KEY"
Write-Host ""
