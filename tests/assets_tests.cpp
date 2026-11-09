#include <QGuiApplication>
#include <QSvgRenderer>
#include <QPainter>
#include <QImage>
#include <QTranslator>
#include <cstdio>
int main(int argc,char **argv) {
    QGuiApplication app(argc,argv);
    if(argc!=3)return 1;
    for(const auto *name : {"flag-us.svg","flag-uk.svg","ai-assistant.svg"}) {
        QSvgRenderer svg(QString::fromLocal8Bit(argv[1])+"/"+name);
        if(!svg.isValid())return 2;
        QImage image(80,50,QImage::Format_ARGB32);image.fill(Qt::transparent);
        QPainter painter(&image);svg.render(&painter);painter.end();
        bool visible=false;
        for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)visible|=qAlpha(image.pixel(x,y))!=0;
        if(!visible)return 3;
    }
    QTranslator translator;
    if(!translator.load(QString::fromLocal8Bit(argv[2])))return 4;
    if(translator.isEmpty())return 5;
    std::puts("SVG rendering and compiled Russian translation load passed.");
}
