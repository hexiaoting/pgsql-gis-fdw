//
// Created by 何文婷 on 18/3/22.
//

#ifndef _RT_FDW_COMMON_H
#define _RT_FDW_COMMON_H
#include <float.h>
#include <string.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>

#include "gdal.h"
#include "stddef.h"
#include "stdint.h"
#include "postgres.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "ogr_srs_api.h"
#include "gdal_vrt.h"

#define FLT_NEQ(x, y) (fabs(x - y) > FLT_EPSILON)
#define FLT_EQ(x, y) (!FLT_NEQ(x, y))
#define LOCATION_MAXSIZE 512
// Each time fetch how many lines from raster file
#define DEFAULT_BATCHSIZE 100
#define FILENAME_MAXSIZE 50

/* Pixel types */
typedef enum {
    PT_1BB=0,     /* 1-bit boolean            */
    PT_2BUI=1,    /* 2-bit unsigned integer   */
    PT_4BUI=2,    /* 4-bit unsigned integer   */
    PT_8BSI=3,    /* 8-bit signed integer     */
    PT_8BUI=4,    /* 8-bit unsigned integer   */
    PT_16BSI=5,   /* 16-bit signed integer    */
    PT_16BUI=6,   /* 16-bit unsigned integer  */
    PT_32BSI=7,   /* 32-bit signed integer    */
    PT_32BUI=8,   /* 32-bit unsigned integer  */
    PT_32BF=10,   /* 32-bit float             */
    PT_64BF=11,   /* 64-bit float             */
    PT_END=13
} rt_pixtype;

typedef struct rt_raster_t* rt_raster;
typedef struct rt_band_t* rt_band;

struct rt_raster_t {
    uint32_t size;
    uint16_t version;
    /* Number of bands, all share the same dimension
     * and georeference */
    uint16_t numBands;
    /* Georeference (in projection units) */
    double scaleX; /* pixel width */
    double scaleY; /* pixel height */
    double ipX; /* geo x ordinate of the corner of upper-left pixel */
    double ipY; /* geo y ordinate of the corner of bottom-right pixel */
    double skewX; /* skew about the X axis*/
    double skewY; /* skew about the Y axis */

    int32_t srid; /* spatial reference id */
    uint16_t width; /* pixel columns - max 65535 */
    uint16_t height; /* pixel rows - max 65535 */
    rt_band *bands; /* actual bands */
};

struct rt_extband_t {
    uint8_t bandNum; /* 0-based */
    char* path; /* internally owned */
    void *mem; /* loaded external band data, internally owned */
};

struct rt_band_t {
    rt_pixtype pixtype;
    int32_t offline;
    uint16_t width;
    uint16_t height;
    int32_t hasnodata; /* a flag indicating if this band contains nodata values */
    int32_t isnodata;   /* a flag indicating if this band is filled only with
                           nodata values. flag CANNOT be TRUE if hasnodata is FALSE */
    double nodataval; /* int will be converted ... */
    int8_t ownsdata; /* 0, externally owned. 1, internally owned. only applies to data.mem */
    rt_raster raster; /* reference to parent raster */
    union {
        void* mem; /* actual data, externally owned */
        struct rt_extband_t offline;
    } data;
};

typedef enum {
    ES_NONE = 0, /* no error */
    ES_ERROR = 1 /* generic error */
} rt_errorstate;

typedef struct RasterConfig{
    int tile_size[2];
    /* SRID of input raster */
    int srid;
    /* SRID of output raster (reprojection) */
    int out_srid;
    int nband_count;
    int pad_tile;
    char *file_column_name;
    int *nband;
    int hasnodata;
    double nodataval;
    int batchsize;
} RasterConfig;

typedef struct rasterinfo_t {
    /* SRID of raster */
    int srid;
    /* srs of raster */
    char *srs;
    /* width, height */
    uint32_t dim[2];
    /* number of bands */
    int *nband; /* 1-based */
    int nband_count;
    /* array of pixeltypes */
    GDALDataType *gdalbandtype;
    rt_pixtype *bandtype;
    /* array of hasnodata flags */
    int *hasnodata;
    /* array of nodatavals */
    double *nodataval;
    /* geotransform matrix */
    double gt[6];
    /* tile size */
    int tile_size[2];
} RASTERINFO;


extern void rterror(const char *fmt, ...);
extern void rtinfo(const char *fmt, ...);
extern void rtwarn(const char *fmt, ...);
extern void rtdealloc(void * mem);
extern void *rtrealloc(void * mem, size_t size);
extern void *rtalloc(size_t size);
extern void rt_raster_set_srid(rt_raster raster, int32_t srid);
extern rt_errorstate rt_band_load_offline_data(rt_band band);
extern int rt_pixtype_size(rt_pixtype pixtype);
extern int rt_raster_get_num_bands(rt_raster raster);
extern rt_band rt_raster_get_band(rt_raster raster, int n);
extern int rt_band_is_offline(rt_band band);
extern void rt_band_destroy(rt_band band);
extern char *rt_raster_to_hexwkb(rt_raster raster, int outasin, uint32_t *hexwkbsize);
extern uint8_t *rt_raster_to_wkb(rt_raster raster, int outasin, uint32_t *wkbsize);
extern uint8_t isMachineLittleEndian(void);
extern uint16_t rt_raster_get_width(rt_raster raster);
extern uint16_t rt_raster_get_height(rt_raster raster);
extern uint8_t rt_util_clamp_to_1BB(double value);
extern uint8_t rt_util_clamp_to_2BUI(double value);

extern uint8_t rt_util_clamp_to_4BUI(double value);
extern int8_t rt_util_clamp_to_8BSI(double value);
extern uint8_t rt_util_clamp_to_8BUI(double value);
extern int16_t rt_util_clamp_to_16BSI(double value);
extern uint16_t rt_util_clamp_to_16BUI(double value);
extern int32_t rt_util_clamp_to_32BSI(double value);
extern uint32_t rt_util_clamp_to_32BUI(double value);
extern float rt_util_clamp_to_32F(double value);
extern rt_band rt_band_new_inline(
        uint16_t width, uint16_t height,
        rt_pixtype pixtype,
        uint32_t hasnodata, double nodataval,
        uint8_t* data);
extern void rt_band_set_ownsdata_flag(rt_band band, int flag);
extern int rt_raster_add_band(rt_raster raster, rt_band band, int index);
extern rt_errorstate rt_band_set_isnodata_flag(rt_band band, int flag);
extern rt_errorstate rt_band_set_pixel_line(
        rt_band band,
        int x, int y,
        void *vals, uint32_t len);

extern void* rt_band_get_data(rt_band band);
extern void rt_raster_destroy(rt_raster raster);
extern rt_raster rt_raster_new(uint32_t width, uint32_t height);
extern int rt_raster_generate_new_band(
        rt_raster raster,
        rt_pixtype pixtype,
        double initialvalue,
        uint32_t hasnodata, double nodatavalue,
        int index);
extern rt_errorstate rt_util_gdal_sr_auth_info(GDALDatasetH hds, char **authname, char **authcode);
extern int rt_band_get_ownsdata_flag(rt_band band);

rt_pixtype rt_util_gdal_datatype_to_pixtype(GDALDataType gdt);
void raster_destroy(rt_raster raster);
rt_raster rt_raster_from_gdal_dataset(GDALDatasetH ds);

void rtdealloc_config(RasterConfig *config);
void init_config(RasterConfig *config);
void set_raster_config(RasterConfig **config, char *conf_file);

int analysis_raster(char *filename, RasterConfig *config, int cur_lineno, char **buf);
int convert_raster(char *filename, RasterConfig *config, RASTERINFO *info, int cur_lineno, char **buf);
#endif //RASTERDB_LIBRTCORE_H
