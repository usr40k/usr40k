#!/usr/bin/env zsh

echo ""
for img in ./*.{gif,png,jpg,jpeg}(N); do
    if [[ -f "$img" ]]; then
        clean_path="${img#./}"
        echo "<a href=\"media/images/slideshow/$clean_path\"><img src=\"media/images/slideshow/$clean_path\" alt=\"slideshow item\"></a>"
    fi
done
