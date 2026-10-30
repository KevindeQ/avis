#include "viewport.h"

#include <QFile>
#include <QQuickWindow>
#include <QSGRenderNode>

#include <rhi/qrhi.h>

class viewport_render_node : public QSGRenderNode
{
public:
    viewport_render_node(QQuickWindow* window);

    void set_size(QSize new_size);

    void prepare() override;
    void render(const RenderState* state) override;
    void releaseResources() override;

    RenderingFlags flags() const override;
    QSGRenderNode::StateFlags changedStates() const override;

private:
    void clear_image(QRhiResourceUpdateBatch* update_batch, QRhiTexture* image, const QColor& color);

    QQuickWindow* window_;

    std::unique_ptr<QRhiGraphicsPipeline> pipeline_;
    std::unique_ptr<QRhiShaderResourceBindings> resource_bindings_;
    std::vector<QRhiShaderStage> shaders_;

    std::unique_ptr<QRhiTexture> image_;
    std::unique_ptr<QRhiSampler> sampler_;
    QSize image_size_;
};

viewport_render_node::viewport_render_node(QQuickWindow* window) : window_(window)
{
    QFile file;
    file.setFileName(":/avis/visualizer/shaders/fullscreen_triangle.vert.qsb");
    if (!file.open(QFile::ReadOnly))
    {
        qFatal("Failed to load vertex shader");
    }
    shaders_.push_back(QRhiShaderStage{ QRhiShaderStage::Vertex, QShader::fromSerialized(file.readAll()) });

    file.close();
    file.setFileName(":/avis/visualizer/shaders/fullscreen_texture.frag.qsb");
    if (!file.open(QFile::ReadOnly))
    {
        qFatal("Failed to load fragment shader");
    }
    shaders_.push_back(QRhiShaderStage{ QRhiShaderStage::Fragment, QShader::fromSerialized(file.readAll()) });
}

void viewport_render_node::set_size(QSize new_size)
{
    image_size_ = new_size;

    // Only reset the underlying graphics object if it was already prepared. This ensures the graphics stack is ready
    // for creating the texture.
    if (image_)
    {
        image_->setPixelSize(new_size);
        image_->create();

        // Clear the newly sized image to magenta
        QRhi* rhi = window_->rhi();
        QRhiResourceUpdateBatch* update_batch = rhi->nextResourceUpdateBatch();
        clear_image(update_batch, image_.get(), QColor{ Qt::magenta });
        commandBuffer()->resourceUpdate(update_batch);
    }
}

void viewport_render_node::prepare()
{
    QRhi* rhi = window_->rhi();
    QRhiResourceUpdateBatch* update_batch = rhi->nextResourceUpdateBatch();

    if (!image_)
    {
        image_.reset(rhi->newTexture(QRhiTexture::RGBA8, image_size_));
        image_->create();

        // Clear the image to magenta
        clear_image(update_batch, image_.get(), QColor{ Qt::magenta });
    }

    if (!sampler_)
    {
        sampler_.reset(rhi->newSampler(
            QRhiSampler::Linear,
            QRhiSampler::Linear,
            QRhiSampler::Linear,
            QRhiSampler::ClampToEdge,
            QRhiSampler::ClampToEdge));
        sampler_->create();
    }

    if (!resource_bindings_)
    {
        resource_bindings_.reset(rhi->newShaderResourceBindings());
        resource_bindings_->setBindings(
            { QRhiShaderResourceBinding::sampledTexture(
                0, QRhiShaderResourceBinding::FragmentStage, image_.get(), sampler_.get()) });
        resource_bindings_->create();
    }

    if (!pipeline_)
    {
        pipeline_.reset(rhi->newGraphicsPipeline());

        // If layer.enabled == true on our QQuickItem and layer.textureMirroring is the
        // default MirrorVertically, the winding order needs to be inverted when
        // isYUpInFrameBuffer == true (OpenGL). This does not happen with other backends,
        // unless textureMirroring is changed so that the situation is inverted, meaning
        // GL needs no adjustments while others need the opposite winding order. Here we
        // choose the replicate, to a degree, what the scenegraph renderer does, but note that
        // this is only correct as long as textureMirroring is not changed from the default.
        pipeline_->setFrontFace(
            renderTarget()->resourceType() == QRhiResource::TextureRenderTarget && rhi->isYUpInFramebuffer() ?
                QRhiGraphicsPipeline::CW :
                QRhiGraphicsPipeline::CCW);
        pipeline_->setCullMode(QRhiGraphicsPipeline::Back);
        pipeline_->setTopology(QRhiGraphicsPipeline::TriangleStrip);

        QRhiGraphicsPipeline::TargetBlend blend;
        blend.enable = false;
        pipeline_->setTargetBlends({ blend });

        pipeline_->setShaderResourceBindings(resource_bindings_.get());
        pipeline_->setShaderStages(shaders_.cbegin(), shaders_.cend());
        pipeline_->setDepthTest(false);

        pipeline_->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
        pipeline_->create();
    }

    commandBuffer()->resourceUpdate(update_batch);
}

void viewport_render_node::render(const RenderState*)
{
    QRhiCommandBuffer* command_buffer = commandBuffer();

    command_buffer->setGraphicsPipeline(pipeline_.get());

    QSize renderTargetSize = renderTarget()->pixelSize();
    command_buffer->setViewport(QRhiViewport(0, 0, renderTargetSize.width(), renderTargetSize.height()));

    command_buffer->setShaderResources();
    command_buffer->draw(3);
}

void viewport_render_node::releaseResources() {}

QSGRenderNode::RenderingFlags viewport_render_node::flags() const
{
    // TODO: Which flags?
    return QSGRenderNode::NoExternalRendering | QSGRenderNode::DepthAwareRendering;
}

QSGRenderNode::StateFlags viewport_render_node::changedStates() const
{
    // In Qt 6 only ViewportState and ScissorState matter, the rest is ignored.
    return QSGRenderNode::StateFlag::ViewportState | QSGRenderNode::StateFlag::CullState;
}

void viewport_render_node::clear_image(QRhiResourceUpdateBatch* update_batch, QRhiTexture* image, const QColor& color)
{
    QImage fill_image(image_size_, QImage::Format_RGBA8888);
    fill_image.fill(color);
    update_batch->uploadTexture(image, fill_image);
}

viewport::viewport(QQuickItem* parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
}

QSGNode* viewport::updatePaintNode(QSGNode* old, UpdatePaintNodeData*)
{
    viewport_render_node* node = static_cast<viewport_render_node*>(old);
    if (node == nullptr)
    {
        node = new viewport_render_node(window());
    }

    node->set_size(size().toSize());

    return node;
}
