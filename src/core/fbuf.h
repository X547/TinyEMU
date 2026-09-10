#ifndef FBUF_H
#define FBUF_H

typedef struct {
    uint8_t *data;
    size_t allocated_size;
} FileBuffer;

void file_buffer_init(FileBuffer *bs);
void file_buffer_reset(FileBuffer *bs);
int file_buffer_resize(FileBuffer *bs, size_t new_size);
void file_buffer_write(FileBuffer *bs, size_t offset, const uint8_t *buf,
                       size_t size);
void file_buffer_set(FileBuffer *bs, size_t offset, int val, size_t size);
void file_buffer_read(FileBuffer *bs, size_t offset, uint8_t *buf,
                      size_t size);

#endif /* FBUF_H */
