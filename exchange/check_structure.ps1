$content = Get-Content "main.cpp"
$pat = "^namespace|^\\|$|^int main|^void clear_screen|^void print_plot_line|^}  // namespace"
for ($i = 0; $i -lt $content.Length; $i++) {
    $line = $content[$i]
    if ($line -match $pat) {
        Write-Host ("Line {0,3}: {1}" -f ($i+1), $line)
    }
}
