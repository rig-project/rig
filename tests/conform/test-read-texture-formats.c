#include <config.h>

#include <cogl/cogl.h>
#include <stdarg.h>

#include "test-utils.h"

/*
 * This tests reading back an RGBA texture in all of the available
 * pixel formats
 */

static const uint8_t tex_data[4] = { 0x12, 0x34, 0x56, 0x78 };

static void
test_read_byte (cg_texture_2d_t *tex_2d,
                cg_pixel_format_t format,
                uint8_t expected_byte)
{
  uint8_t received_byte;

  cg_texture_get_data (tex_2d,
                         format,
                         1, /* rowstride */
                         &received_byte);

  c_assert_cmpint (expected_byte, ==, received_byte);
}

static void
test_read_short (cg_texture_2d_t *tex_2d,
                 cg_pixel_format_t format,
                 ...)
{
  va_list ap;
  int bits;
  uint16_t received_value;
  uint16_t expected_value = 0;
  char *received_value_str;
  char *expected_value_str;
  int bits_sum = 0;

  cg_texture_get_data (tex_2d,
                         format,
                         2, /* rowstride */
                         (uint8_t *) &received_value);

  va_start (ap, format);

  /* Convert the va args into a single 16-bit expected value */
  while ((bits = va_arg (ap, int)) != -1)
    {
      int value = (va_arg (ap, int) * ((1 << bits) - 1) + 128) / 255;

      bits_sum += bits;

      expected_value |= value << (16 - bits_sum);
    }

  va_end (ap);

  received_value_str = c_strdup_printf ("0x%04x", received_value);
  expected_value_str = c_strdup_printf ("0x%04x", expected_value);
  c_assert_cmpstr (received_value_str, ==, expected_value_str);
  c_free (received_value_str);
  c_free (expected_value_str);
}

static void
test_read_888 (cg_texture_2d_t *tex_2d,
               cg_pixel_format_t format,
               uint32_t expected_pixel)
{
  uint8_t pixel[4];

  cg_texture_get_data (tex_2d,
                         format,
                         4, /* rowstride */
                         pixel);

  test_utils_compare_pixel (pixel, expected_pixel);
}

static void
test_read_88 (cg_texture_2d_t *tex_2d,
              cg_pixel_format_t format,
              uint32_t expected_pixel)
{
  uint8_t pixel[4];

  pixel[2] = 0x00;

  cg_texture_get_data (tex_2d,
                         format,
                         2, /* rowstride */
                         pixel);

  test_utils_compare_pixel (pixel, expected_pixel);
}

static void
test_read_8888 (cg_texture_2d_t *tex_2d,
                cg_pixel_format_t format,
                uint32_t expected_pixel)
{
  uint32_t received_pixel;
  char *received_value_str;
  char *expected_value_str;

  cg_texture_get_data (tex_2d,
                         format,
                         4, /* rowstride */
                         (uint8_t *) &received_pixel);

  received_pixel = C_UINT32_FROM_BE (received_pixel);

  received_value_str = c_strdup_printf ("0x%08x", received_pixel);
  expected_value_str = c_strdup_printf ("0x%08x", expected_pixel);
  c_assert_cmpstr (received_value_str, ==, expected_value_str);
  c_free (received_value_str);
  c_free (expected_value_str);
}

static void
test_read_int (cg_texture_2d_t *tex_2d,
               cg_pixel_format_t format,
               ...)
{
  va_list ap;
  int bits;
  uint32_t received_value;
  uint32_t expected_value = 0;
  char *received_value_str;
  char *expected_value_str;
  int bits_sum = 0;

  cg_texture_get_data (tex_2d,
                         format,
                         4, /* rowstride */
                         (uint8_t *) &received_value);

  va_start (ap, format);

  /* Convert the va args into a single 32-bit expected value */
  while ((bits = va_arg (ap, int)) != -1)
    {
      uint32_t value = (va_arg (ap, int) * ((1 << bits) - 1) + 128) / 255;

      bits_sum += bits;

      expected_value |= value << (32 - bits_sum);
    }

  va_end (ap);

  received_value_str = c_strdup_printf ("0x%08x", received_value);
  expected_value_str = c_strdup_printf ("0x%08x", expected_value);
  c_assert_cmpstr (received_value_str, ==, expected_value_str);
  c_free (received_value_str);
  c_free (expected_value_str);
}

void
test_read_texture_formats (void)
{
  cg_texture_2d_t *tex_2d;

  tex_2d = cg_texture_2d_new_from_data (test_dev,
                                          1, 1, /* width / height */
                                          CG_PIXEL_FORMAT_RGBA_8888_PRE,
                                          4, /* rowstride */
                                          tex_data,
                                          NULL);

  test_read_byte (tex_2d, CG_PIXEL_FORMAT_A_8, 0x78);

  /* We should always be able to read into an RG buffer regardless of
   * whether RG textures are supported because Cogl will do the
   * conversion for us */
  test_read_88 (tex_2d, CG_PIXEL_FORMAT_RG_88, 0x123400ff);

  test_read_short (tex_2d, CG_PIXEL_FORMAT_RGB_565,
                   5, 0x12, 6, 0x34, 5, 0x56,
                   -1);
  test_read_short (tex_2d, CG_PIXEL_FORMAT_RGBA_4444_PRE,
                   4, 0x12, 4, 0x34, 4, 0x56, 4, 0x78,
                   -1);
  test_read_short (tex_2d, CG_PIXEL_FORMAT_RGBA_5551_PRE,
                   5, 0x12, 5, 0x34, 5, 0x56, 1, 0x78,
                   -1);

  test_read_888 (tex_2d, CG_PIXEL_FORMAT_RGB_888, 0x123456ff);
  test_read_888 (tex_2d, CG_PIXEL_FORMAT_BGR_888, 0x563412ff);

  test_read_8888 (tex_2d, CG_PIXEL_FORMAT_RGBA_8888_PRE, 0x12345678);
  test_read_8888 (tex_2d, CG_PIXEL_FORMAT_BGRA_8888_PRE, 0x56341278);
  test_read_8888 (tex_2d, CG_PIXEL_FORMAT_ARGB_8888_PRE, 0x78123456);
  test_read_8888 (tex_2d, CG_PIXEL_FORMAT_ABGR_8888_PRE, 0x78563412);

  test_read_int (tex_2d, CG_PIXEL_FORMAT_RGBA_1010102_PRE,
                 10, 0x12, 10, 0x34, 10, 0x56, 2, 0x78,
                 -1);
  test_read_int (tex_2d, CG_PIXEL_FORMAT_BGRA_1010102_PRE,
                 10, 0x56, 10, 0x34, 10, 0x12, 2, 0x78,
                 -1);
  test_read_int (tex_2d, CG_PIXEL_FORMAT_ARGB_2101010_PRE,
                 2, 0x78, 10, 0x12, 10, 0x34, 10, 0x56,
                 -1);
  test_read_int (tex_2d, CG_PIXEL_FORMAT_ABGR_2101010_PRE,
                 2, 0x78, 10, 0x56, 10, 0x34, 10, 0x12,
                 -1);

  cg_object_unref (tex_2d);

  if (cg_test_verbose ())
    c_print ("OK\n");
}
