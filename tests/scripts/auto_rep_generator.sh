
  while true; do 
    ./scripts/test0/plots.sh --srcdir logs --dstdir reports
    ./scripts/test0/stats.sh --srcdir logs --dst reports/stats.csv
    mv logs/ccparams_1.log reports/ccparams_1.log
    ./scripts/test0/report.sh --srcdir reports --author logs/author.txt --dst report.tex
    ./scripts/pdflatex.sh report.tex

    mv report.pdf reports/report.pdf
    sleep 5
  done

  
