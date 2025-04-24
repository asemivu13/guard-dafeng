package org.example;

import org.apache.pdfbox.Loader;
import org.apache.pdfbox.pdmodel.PDDocument;
import org.apache.pdfbox.pdmodel.PDPage;
import org.apache.pdfbox.pdmodel.PDPageContentStream;
import org.apache.pdfbox.pdmodel.font.PDFont;
import org.apache.pdfbox.pdmodel.font.PDType1Font;
import org.apache.pdfbox.pdmodel.font.Standard14Fonts;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.net.URISyntaxException;
import java.net.URL;
import java.nio.file.Path;
import java.nio.file.Paths;

public class Main {
    public static void main(String[] args) throws IOException, URISyntaxException {
        Path path = Paths.get("C:\\Users\\asem\\Downloads\\sample-local-pdf.pdf");
        PDDocument pdDocument = Loader.loadPDF(path.toFile());

        PDPage page = pdDocument.getPage(1);
        PDPageContentStream pdPageContentStream = new PDPageContentStream(pdDocument, page, PDPageContentStream.AppendMode.APPEND, false);
        pdPageContentStream.beginText();
        pdPageContentStream.setFont(new PDType1Font(Standard14Fonts.FontName.HELVETICA), 12);
        pdPageContentStream.showText("hello world");
        pdPageContentStream.endText();
        pdPageContentStream.close();

        pdDocument.save("save-pdf.pdf");
    }
}
