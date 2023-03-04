LATEXMK_IMAGE := danteev/texlive
LATEXMK = docker run --rm -v $$(pwd):/workdir $(LATEXMK_IMAGE) latexmk

all:
	$(LATEXMK) -pdf -shell-escape -use-make main.tex

clean:
	$(LATEXMK) -pdf -C main.tex
	rm *.bbl
