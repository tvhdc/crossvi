#pragma once
#include <HalStorage.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "FootnoteEntry.h"
#include "blocks/ImageBlock.h"
#include "blocks/TextBlock.h"

class BoundedFileReader;
namespace serialization {
class BufferedFileWriter;
}

enum PageElementTag : uint8_t {
  TAG_PageLine = 1,
  TAG_PageImage = 2,
  TAG_PageHorizontalRule = 3,
};

// represents something that has been added to a page
class PageElement {
 public:
  int16_t xPos;
  int16_t yPos;
  explicit PageElement(const int16_t xPos, const int16_t yPos) : xPos(xPos), yPos(yPos) {}
  virtual ~PageElement() = default;
  virtual void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) = 0;
  virtual bool serialize(serialization::BufferedFileWriter& file) = 0;
  virtual PageElementTag getTag() const = 0;  // Add type identification
};

// a line from a block element
class PageLine final : public PageElement {
  std::shared_ptr<TextBlock> block;

 public:
  PageLine(std::shared_ptr<TextBlock> block, const int16_t xPos, const int16_t yPos)
      : PageElement(xPos, yPos), block(std::move(block)) {}
  const std::shared_ptr<TextBlock>& getBlock() const { return block; }
  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) override;
  bool serialize(serialization::BufferedFileWriter& file) override;
  PageElementTag getTag() const override { return TAG_PageLine; }
  static std::unique_ptr<PageLine> deserialize(BoundedFileReader& reader);
};

// New PageImage class
class PageImage final : public PageElement {
  std::shared_ptr<ImageBlock> imageBlock;

 public:
  PageImage(std::shared_ptr<ImageBlock> block, const int16_t xPos, const int16_t yPos)
      : PageElement(xPos, yPos), imageBlock(std::move(block)) {}
  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) override;
  void renderWithScratch(GfxRenderer& renderer, int xOffset, int yOffset, std::unique_ptr<uint8_t[]>& readBuffer,
                         size_t& readBufferCapacity);
  void renderPlaceholder(GfxRenderer& renderer, int xOffset, int yOffset) const;
  bool serialize(serialization::BufferedFileWriter& file) override;
  PageElementTag getTag() const override { return TAG_PageImage; }
  static std::unique_ptr<PageImage> deserialize(BoundedFileReader& reader);
  const ImageBlock& getImageBlock() const { return *imageBlock; }
};

class PageHorizontalRule final : public PageElement {
  uint16_t width;
  uint8_t thickness;

 public:
  PageHorizontalRule(uint16_t width, uint8_t thickness, const int16_t xPos, const int16_t yPos)
      : PageElement(xPos, yPos), width(width), thickness(thickness) {}

  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) override;
  bool serialize(serialization::BufferedFileWriter& file) override;
  PageElementTag getTag() const override { return TAG_PageHorizontalRule; }
  static std::unique_ptr<PageHorizontalRule> deserialize(BoundedFileReader& reader);
};

struct PageImagePreparation {
  std::string sourcePath;
  std::string imagePath;
  int16_t x = 0;
  int16_t y = 0;
  int16_t width = 0;
  int16_t height = 0;
  bool needsExtraction = false;
};

class Page {
  mutable std::unique_ptr<uint8_t[]> imageReadBuffer;
  mutable size_t imageReadBufferCapacity = 0;

 public:
  // the list of block index and line numbers on this page
  std::vector<std::shared_ptr<PageElement>> elements;
  std::vector<FootnoteEntry> footnotes;
  static constexpr uint16_t MAX_FOOTNOTES_PER_PAGE = 16;

  void addFootnote(const char* number, const char* href) {
    if (footnotes.size() >= MAX_FOOTNOTES_PER_PAGE) return;  // Cap per-page footnotes
    FootnoteEntry entry;
    strncpy(entry.number, number, sizeof(entry.number) - 1);
    entry.number[sizeof(entry.number) - 1] = '\0';
    strncpy(entry.href, href, sizeof(entry.href) - 1);
    entry.href[sizeof(entry.href) - 1] = '\0';
    footnotes.push_back(entry);
  }

  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) const;
  void renderImages(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) const;
  void renderWithImagePlaceholders(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) const;
  bool serialize(HalFile& file, uint8_t* scratchBuffer = nullptr, size_t scratchCapacity = 0) const;
  static std::unique_ptr<Page> deserialize(BoundedFileReader& reader);

  // Check if page contains any images (used to force full refresh)
  bool hasImages() const {
    return std::any_of(elements.begin(), elements.end(),
                       [](const std::shared_ptr<PageElement>& el) { return el->getTag() == TAG_PageImage; });
  }

  bool hasImagesNeedingDecode() const {
    return std::any_of(elements.begin(), elements.end(), [](const std::shared_ptr<PageElement>& element) {
      return element->getTag() == TAG_PageImage &&
             static_cast<const PageImage&>(*element).getImageBlock().needsDecode();
    });
  }

  bool hasImagesDecodedWithoutCache() const {
    return std::any_of(elements.begin(), elements.end(), [](const std::shared_ptr<PageElement>& element) {
      return element->getTag() == TAG_PageImage &&
             static_cast<const PageImage&>(*element).getImageBlock().wasDecodedWithoutCache();
    });
  }

  // Returns one raster that still needs raw extraction or pixel-cache decode.
  // The caller owns elementIndex so idle preparation never retains a Page or
  // an unbounded candidate list.
  bool nextImageNeedingPreparation(size_t& elementIndex, PageImagePreparation& candidate) const {
    while (elementIndex < elements.size()) {
      const auto& element = elements[elementIndex++];
      if (element->getTag() != TAG_PageImage) continue;
      const auto& pageImage = static_cast<const PageImage&>(*element);
      const auto& image = pageImage.getImageBlock();
      if (!image.needsDecode()) continue;
      const bool needsExtraction = !image.imageExists();
      if (needsExtraction && image.getSourcePath().empty()) continue;
      candidate.sourcePath = image.getSourcePath();
      candidate.imagePath = image.getImagePath();
      candidate.x = pageImage.xPos;
      candidate.y = pageImage.yPos;
      candidate.width = image.getWidth();
      candidate.height = image.getHeight();
      candidate.needsExtraction = needsExtraction;
      return true;
    }
    return false;
  }

  // Used only for the optional EPUB opening-page skip. A page with no text,
  // rule or footnote and one image is safe to classify from serialized
  // metadata, before any image is extracted or decoded.
  bool isImageOnly() const { return footnotes.empty() && elements.size() == 1 && hasImages(); }

  // A strict cover-only page has no text/rule or second image. The source href
  // is normalized by ChapterHtmlSlimParser and compared with OPF metadata, so
  // ambiguous or malformed EPUBs fail open and keep their page visible.
  bool isCoverOnly(const std::string& coverHref) const {
    if (coverHref.empty() || !footnotes.empty() || elements.size() != 1 ||
        elements.front()->getTag() != TAG_PageImage) {
      return false;
    }
    const auto& image = static_cast<const PageImage&>(*elements.front()).getImageBlock();
    return image.getSourcePath() == coverHref;
  }

  // Get bounding box of all images on the page (union of image rects)
  // Returns false if no images. Coordinates are relative to page origin.
  bool getImageBoundingBox(int16_t& outX, int16_t& outY, int16_t& outW, int16_t& outH) const {
    bool found = false;
    int16_t minX = INT16_MAX, minY = INT16_MAX, maxX = INT16_MIN, maxY = INT16_MIN;
    for (const auto& el : elements) {
      if (el->getTag() == TAG_PageImage) {
        const auto& img = static_cast<const PageImage&>(*el);
        int16_t x = img.xPos;
        int16_t y = img.yPos;
        int16_t right = x + img.getImageBlock().getWidth();
        int16_t bottom = y + img.getImageBlock().getHeight();
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        maxX = std::max(maxX, right);
        maxY = std::max(maxY, bottom);
        found = true;
      }
    }
    if (found) {
      outX = minX;
      outY = minY;
      outW = maxX - minX;
      outH = maxY - minY;
    }
    return found;
  }
};
