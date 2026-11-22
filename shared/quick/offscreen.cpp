#include "offscreen.h"
#include <QQuickRenderControl>
#include <QQuickWindow>
#include <QQuickItem>
#include <QQuickRenderTarget>
#include <QQuickGraphicsConfiguration>
#include <QVulkanInstance>
#include <rhi/qrhi.h>
namespace SuiteQuick {
struct Offscreen::Data {
    QSize size;
    QVulkanInstance instance;
    QQuickRenderControl control;
    QQuickWindow window{&control};
    std::unique_ptr<QRhiTexture> texture;
    std::unique_ptr<QRhiRenderBuffer> depth;
    std::unique_ptr<QRhiTextureRenderTarget> target;
    std::unique_ptr<QRhiRenderPassDescriptor> pass;
    QImage software;
    bool cpu=false,initialized=false;
    ~Data(){control.invalidate();window.setRenderTarget({});target.reset();pass.reset();depth.reset();texture.reset();}
};
Offscreen::Offscreen(const QSize &size):d(std::make_unique<Data>()){d->size=size;d->window.resize(size);d->window.contentItem()->setSize(size);d->window.setColor(Qt::black);}
Offscreen::~Offscreen()=default;
QQuickWindow *Offscreen::window()const{return &d->window;}
bool Offscreen::initialize(QString *error) {
    if(error)error->clear();
    const auto fail=[&](const QString &why){if(error)*error=why;return false;};
    if(d->initialized)return true;
    if(d->size.isEmpty() || qint64(d->size.width())*d->size.height()>16777216)return fail("Invalid offscreen image size.");
    d->cpu=d->window.rendererInterface()->graphicsApi()==QSGRendererInterface::Software;
    if(d->cpu){d->software=QImage(d->size,QImage::Format_ARGB32_Premultiplied);if(d->software.isNull())return fail("Cannot allocate software render target.");d->window.setRenderTarget(QQuickRenderTarget::fromPaintDevice(&d->software));}
    else {
        if(QQuickWindow::graphicsApi()==QSGRendererInterface::Vulkan){
            d->instance.setExtensions(QQuickGraphicsConfiguration::preferredInstanceExtensions());
            if(!d->instance.create())return fail("Cannot create the Qt Vulkan instance.");
            d->window.setVulkanInstance(&d->instance);
        }
        if(!d->control.initialize() || !d->control.rhi())return fail("Cannot initialize the Qt Quick graphics device.");
        auto *rhi=d->control.rhi();
        d->texture.reset(rhi->newTexture(QRhiTexture::RGBA8,d->size,1,QRhiTexture::RenderTarget|QRhiTexture::UsedAsTransferSource));
        if(!d->texture->create())return fail("Cannot allocate the Qt Quick colour target.");
        d->depth.reset(rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil,d->size));
        if(!d->depth->create())return fail("Cannot allocate the Qt Quick depth target.");
        QRhiTextureRenderTargetDescription description(QRhiColorAttachment(d->texture.get()));description.setDepthStencilBuffer(d->depth.get());
        d->target.reset(rhi->newTextureRenderTarget(description));d->pass.reset(d->target->newCompatibleRenderPassDescriptor());d->target->setRenderPassDescriptor(d->pass.get());
        if(!d->target->create())return fail("Cannot create the Qt Quick render target.");
        d->window.setRenderTarget(QQuickRenderTarget::fromRhiRenderTarget(d->target.get()));
    }
    d->initialized=true;return true;
}
QImage Offscreen::render(QString *error) {
    if(!initialize(error))return {};
    d->control.polishItems();
    if(d->cpu){d->software.fill(Qt::black);d->control.sync();d->control.render();return d->software.copy();}
    d->control.beginFrame();d->control.sync();d->control.render();
    QRhiReadbackResult result;bool completed=false;result.completed=[&]{completed=true;};
    auto *batch=d->control.rhi()->nextResourceUpdateBatch();batch->readBackTexture(QRhiReadbackDescription(d->texture.get()),&result);d->control.commandBuffer()->resourceUpdate(batch);
    d->control.endFrame();
    if(!completed || result.data.size()!=d->size.width()*d->size.height()*4){if(error)*error="Qt Quick image readback failed.";return {};}
    QImage image(reinterpret_cast<const uchar*>(result.data.constData()),d->size.width(),d->size.height(),QImage::Format_RGBA8888_Premultiplied);
    return d->control.rhi()->isYUpInFramebuffer()?image.flipped(Qt::Vertical):image.copy();
}
QVariantMap Offscreen::information()const {
    const auto api=d->window.rendererInterface()->graphicsApi();
    QVariantMap info{{"renderer",api==QSGRendererInterface::Vulkan?"Vulkan":api==QSGRendererInterface::OpenGL?"OpenGL":api==QSGRendererInterface::Software?"Software":"Other"},{"initialized",d->initialized}};
    if(auto *rhi=d->control.rhi()){auto driver=rhi->driverInfo();info["device"]=QString::fromUtf8(driver.deviceName);info["deviceType"]=int(driver.deviceType);}
    return info;
}
}
