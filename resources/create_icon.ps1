Add-Type -AssemblyName System.Drawing

$pngPath = "d:\work\project\LiveAssistant\resources\logo.png"
$icoPath = "d:\work\project\LiveAssistant\resources\app_icon.ico"

try {
    $img = [System.Drawing.Image]::FromFile($pngPath)
    $icon = [System.Drawing.Icon]::FromHandle($img.GetHicon())
    $stream = [System.IO.File]::OpenWrite($icoPath)
    $icon.Save($stream)
    $stream.Close()
    $icon.Dispose()
    $img.Dispose()
    Write-Host "Successfully created app_icon.ico"
} catch {
    Write-Host "Error: $_"
}
