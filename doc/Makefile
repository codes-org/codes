PDFS := codes-best-practices.pdf

all: $(PDFS)
	@ echo REMAINING TODO ITEMS : `grep TODO codes-best-practices.tex | wc -l`

figs::
	$(MAKE) -C figs

%.pdf: %.eps
	epstopdf $<

%.pdf: %.tex 
	pdflatex $<
	pdflatex $<
	pdflatex $<

codes-best-practices.pdf: codes-best-practices.tex 
	pdflatex $<
	pdflatex $<

clean::
	rm -f $(PDFS) *.dvi *.aux *.bbl *.log *.bak *.toc *.blg *.lof *.ps
