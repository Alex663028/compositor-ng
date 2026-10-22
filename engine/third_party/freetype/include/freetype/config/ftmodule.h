/*
 * This file registers the FreeType modules compiled into the library.
 *
 * Trimmed for compositor-ng (see ../../../VENDOR-NOTES.md):
 *   TrueType + CFF/OpenType + SFNT tables + grayscale rasterizer.
 * The bundled Noto Sans SC is CFF-based OpenType, so `cff' must stay;
 * `truetype' covers .ttf fallbacks.  autofit, type1, cid, pfr, type42,
 * winfonts, pcf, bdf, raster1, sdf and svg are not vendored, so they must
 * not be referenced here — FT_USE_MODULE of a missing class is a link error.
 *
 * Keep the order below: font drivers first, auxiliary modules after, the
 * rasterizer last.  FreeType tests formats in this order.
 */

FT_USE_MODULE( FT_Driver_ClassRec, tt_driver_class )
FT_USE_MODULE( FT_Driver_ClassRec, cff_driver_class )
FT_USE_MODULE( FT_Module_Class, psaux_module_class )
FT_USE_MODULE( FT_Module_Class, psnames_module_class )
FT_USE_MODULE( FT_Module_Class, pshinter_module_class )
FT_USE_MODULE( FT_Module_Class, sfnt_module_class )
FT_USE_MODULE( FT_Renderer_Class, ft_smooth_renderer_class )

/* EOF */
