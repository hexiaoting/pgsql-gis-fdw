//
// Created by 何文婷 on 18/3/22.
//
#include <assert.h>
#include "rt_fdw_common.h"
#include "stdint.h"
#include "ogr_srs_api.h"
#include "gdal_vrt.h"

rt_pixtype
rt_util_gdal_datatype_to_pixtype(GDALDataType gdt) {
    switch (gdt) {
        case GDT_Byte:
            return PT_8BUI;
        case GDT_UInt16:
            return PT_16BUI;
        case GDT_Int16:
            return PT_16BSI;
        case GDT_UInt32:
            return PT_32BUI;
        case GDT_Int32:
            return PT_32BSI;
        case GDT_Float32:
            return PT_32BF;
        case GDT_Float64:
            return PT_64BF;
        default:
            return PT_END;
    }

    return PT_END;
}

void raster_destroy(rt_raster raster) {
    uint16_t i;
    uint16_t nbands = rt_raster_get_num_bands(raster);
    for (i = 0; i < nbands; i++) {
        rt_band band = rt_raster_get_band(raster, i);
        if (band == NULL) continue;

        if (!rt_band_is_offline(band) && !rt_band_get_ownsdata_flag(band)) {
            void* mem = rt_band_get_data(band);
            if (mem) rtdealloc(mem);
        }
        rt_band_destroy(band);
    }
    rt_raster_destroy(raster);
}

rt_raster
rt_raster_from_gdal_dataset(GDALDatasetH ds) {
    rt_raster rast = NULL;
    double gt[6] = {0};
    CPLErr cplerr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t numBands = 0;
    int i = 0;
    char *authname = NULL;
    char *authcode = NULL;

    GDALRasterBandH gdband = NULL;
    GDALDataType gdpixtype = GDT_Unknown;
    rt_band band;
    int32_t idx;
    rt_pixtype pt = PT_END;
    uint32_t ptlen = 0;
    int hasnodata = 0;
    double nodataval;

    int x;
    int y;

    int nXBlocks, nYBlocks;
    int nXBlockSize, nYBlockSize;
    int iXBlock, iYBlock;
    int nXValid, nYValid;
    int iY;

    uint8_t *values = NULL;
    uint32_t valueslen = 0;
    uint8_t *ptr = NULL;


    /* raster size */
    width = GDALGetRasterXSize(ds);
    height = GDALGetRasterYSize(ds);

    /* create new raster */
    rast = rt_raster_new(width, height);
    if (NULL == rast) {
        rterror("rt_raster_from_gdal_dataset: Out of memory allocating new raster");
        return NULL;
    }

    /* get raster attributes */
    cplerr = GDALGetGeoTransform(ds, gt);
    if (cplerr != CE_None) {
//        RASTER_DEBUG(4, "Using default geotransform matrix (0, 1, 0, 0, 0, -1)");
        gt[0] = 0;
        gt[1] = 1;
        gt[2] = 0;
        gt[3] = 0;
        gt[4] = 0;
        gt[5] = -1;
    }

    /* apply raster attributes */
    rast->ipX = gt[0];
    rast->scaleX = gt[1];
    rast->skewX = gt[2];
    rast->ipY = gt[3];
    rast->skewY = gt[4];
    rast->scaleY = gt[5];

    /* srid */
    if (rt_util_gdal_sr_auth_info(ds, &authname, &authcode) == ES_NONE) {
        if (
                authname != NULL &&
                strcmp(authname, "EPSG") == 0 &&
                authcode != NULL
                ) {
            rt_raster_set_srid(rast, atoi(authcode));
        }

        if (authname != NULL)
            rtdealloc(authname);
        if (authcode != NULL)
            rtdealloc(authcode);
    }

    numBands = GDALGetRasterCount(ds);


    /* copy bands */
    for (i = 1; i <= numBands; i++) {
        gdband = NULL;
        gdband = GDALGetRasterBand(ds, i);

        /* pixtype */
        gdpixtype = GDALGetRasterDataType(gdband);
        pt = rt_util_gdal_datatype_to_pixtype(gdpixtype);
        if (pt == PT_END) {
            rterror("rt_raster_from_gdal_dataset: Unknown pixel type for GDAL band");
            rt_raster_destroy(rast);
            return NULL;
        }
        ptlen = rt_pixtype_size(pt);

        /* size: width and height */
        width = GDALGetRasterBandXSize(gdband);
        height = GDALGetRasterBandYSize(gdband);

        /* nodata */
        nodataval = GDALGetRasterNoDataValue(gdband, &hasnodata);

        /* create band object */
        idx = rt_raster_generate_new_band(
                rast, pt,
                (hasnodata ? nodataval : 0),
                hasnodata, nodataval, rt_raster_get_num_bands(rast)
        );
        if (idx < 0) {
            rterror("rt_raster_from_gdal_dataset: Could not allocate memory for raster band");
            rt_raster_destroy(rast);
            return NULL;
        }
        band = rt_raster_get_band(rast, idx);

        /* this makes use of GDAL's "natural" blocks */
        GDALGetBlockSize(gdband, &nXBlockSize, &nYBlockSize);
        nXBlocks = (width + nXBlockSize - 1) / nXBlockSize;
        nYBlocks = (height + nYBlockSize - 1) / nYBlockSize;

        /* allocate memory for values */
        valueslen = ptlen * nXBlockSize * nYBlockSize;
        values = rtalloc(valueslen);
        if (values == NULL) {
            rterror("rt_raster_from_gdal_dataset: Could not allocate memory for GDAL band pixel values");
            rt_raster_destroy(rast);
            return NULL;
        }

        for (iYBlock = 0; iYBlock < nYBlocks; iYBlock++) {
            for (iXBlock = 0; iXBlock < nXBlocks; iXBlock++) {
                x = iXBlock * nXBlockSize;
                y = iYBlock * nYBlockSize;

                memset(values, 0, valueslen);

                /* valid block width */
                if ((iXBlock + 1) * nXBlockSize > width)
                    nXValid = width - (iXBlock * nXBlockSize);
                else
                    nXValid = nXBlockSize;

                /* valid block height */
                if ((iYBlock + 1) * nYBlockSize > height)
                    nYValid = height - (iYBlock * nYBlockSize);
                else
                    nYValid = nYBlockSize;

                cplerr = GDALRasterIO(
                        gdband, GF_Read,
                        x, y,
                        nXValid, nYValid,
                        values, nXValid, nYValid,
                        gdpixtype,
                        0, 0
                );
                if (cplerr != CE_None) {
                    rterror("rt_raster_from_gdal_dataset: Could not get data from GDAL raster");
                    rtdealloc(values);
                    rt_raster_destroy(rast);
                    return NULL;
                }

                /* if block width is same as raster width, shortcut */
                if (nXBlocks == 1 && nYBlockSize > 1 && nXValid == width) {
                    x = 0;
                    y = nYBlockSize * iYBlock;

                    rt_band_set_pixel_line(band, x, y, values, nXValid * nYValid);
                }
                else {
                    ptr = values;
                    x = nXBlockSize * iXBlock;
                    for (iY = 0; iY < nYValid; iY++) {
                        y = iY + (nYBlockSize * iYBlock);

                        rt_band_set_pixel_line(band, x, y, ptr, nXValid);
                        ptr += (nXValid * ptlen);
                    }
                }
            }
        }

        /* free memory */
        rtdealloc(values);
    }

    return rast;
}

void init_config(RasterConfig *config) {
    config->srid = config->out_srid = 0;
    config->batchsize = DEFAULT_BATCHSIZE;
    config->nband = NULL;
    config->nband_count = 0;
    memset(config->tile_size, 0, sizeof(int) * 2);
    config->pad_tile = 0;
    config->hasnodata = 0;
    config->nodataval = 0;
}

void set_raster_config(RasterConfig **config, char *conf_file) {
    int MAXSIZE=1024;
    char buf[MAXSIZE];
    FILE *f = NULL;

    //S1: set config
    *config = rtalloc(sizeof(RasterConfig));
    if (*config == NULL) {
        elog(ERROR, "rtalloc RasterConfig failed");
    }
    init_config(*config);

    Assert(conf_file != NULL);
    f = fopen(conf_file, "r");
    if (f == NULL) {
        elog(ERROR, "open %s failed. errno=%s", conf_file, strerror(errno));
    }
    memset(buf, 0, MAXSIZE);
    while (fgets(buf, MAXSIZE, f) != NULL) {
        char *p = strchr(buf, '=');
        if (p == NULL) {
            elog(INFO, "conf_file setting %s failed(without '=').", buf);
        }
        else if (strncmp(buf,"tile_size",strlen("tile_size")) == 0) {
            char *p2 = strchr(p, 'x');
            if (p2 != NULL) {
                char s1[5];
                strncpy(s1,p+1,p2-p);
                (*config)->tile_size[0] = atoi(s1);
                (*config)->tile_size[1] = atoi(p2+1);
                elog(DEBUG1, "config->tile_size=%dx%d",(*config)->tile_size[0],(*config)->tile_size[1]);
            }
        } else if(strncmp(buf, "batchsize", strlen("batchsize")) == 0) {
            (*config)->batchsize = atoi(p + 1);
            elog(DEBUG1, "config->batchsize= %d", (*config)->batchsize);
        }
    }
    fclose(f);
}

void rtdealloc_config(RasterConfig *config) {
    if (config->nband_count > 0 && config->nband != NULL)
        rtdealloc(config->nband);
    rtdealloc(config);
}

int analysis_raster(char *filename, RasterConfig *config, int cur_lineno, char **buf) {
    int rows = 0;
    RASTERINFO *rasterinfo;

    elog(DEBUG1, "----->analysis_raster(%s:%d)", filename, cur_lineno);
    rasterinfo = rtalloc(sizeof(RASTERINFO));
    if(rasterinfo == NULL) {
        elog(ERROR, "rtalloc for RASTERINFO failed");
    }
    memset(rasterinfo, 0, sizeof(RASTERINFO));

    /* convert raster to tiles and explained in hexString*/
    rows = convert_raster(filename, config, rasterinfo, cur_lineno, buf);

    elog(DEBUG1, "<-----analysis_raster");
    return rows;
}

int convert_raster(char *filename, RasterConfig *config, RASTERINFO *info, int cur_lineno, char **buf) {
    int ntiles[2] = {1, 1};
    int tileno = 0;
    int processdno = 0;
    int _tile_size[2] = {0};
    int i = 0, xtile = 0, ytile = 0;
    int nbands = 0;
    int batchsize = config->batchsize;
    uint32_t hexlen = 0;
    double gt[6] = {0.};
    char *hex = NULL;
    const char *proDefString = NULL;
    GDALDatasetH hds;
    VRTDatasetH tds;
    VRTSourcedRasterBandH tband;
    rt_raster rast = NULL;

    elog(DEBUG1, "----->convert_raster");
    hds = GDALOpen(filename, GA_ReadOnly);

    //S1: get tilesize
    // dimensions of raster
    info->dim[0] = GDALGetRasterXSize(hds);
    info->dim[1] = GDALGetRasterYSize(hds);

    /* tile split:
     * if no tile size set, then reuse orignal raster dimensions
     * eg: raster dimenstion = 400x400
     *       tile dimenstion = 100x100
     *       then there where be 160000/10000 = 16 number of tiles
     * */
    info->tile_size[0] = (config->tile_size[0] ? config->tile_size[0] : info->dim[0]);
    info->tile_size[1] = (config->tile_size[1] ? config->tile_size[1] : info->dim[1]);
    // number of tiles on width and height
    if (config->tile_size[0] != info->dim[0])
        ntiles[0] = (info->dim[0] + info->tile_size[0] - 1)/(info->tile_size[0]);
    if (config->tile_size[1] != info->dim[1])
        ntiles[1] = (info->dim[1] + info->tile_size[1] - 1)/(info->tile_size[1]);

    tileno = ntiles[0] * ntiles[1];
    if(tileno < cur_lineno)
        return 0;

    //S2: get srs and srid
    proDefString =  GDALGetProjectionRef(hds);
    if (proDefString != NULL && proDefString[0] != '\0') {
        OGRSpatialReferenceH hSRS = OSRNewSpatialReference(NULL);
        //Set info->srs
        info->srs = rtalloc(sizeof(char *) * (strlen(proDefString) + 1));
        if (info->srs == NULL) {
            GDALClose(hds);
            elog(ERROR, "rtalloc for info->srs failed.");
        }
        strcpy(info->srs, proDefString);

        //Set info->srid
        if (OSRSetFromUserInput(hSRS, proDefString) == OGRERR_NONE) {
            const char* pszAuthorityName = OSRGetAuthorityName(hSRS, NULL);
            const char* pszAuthorityCode = OSRGetAuthorityCode(hSRS, NULL);
            if (pszAuthorityName != NULL &&
                    strcmp(pszAuthorityName, "EPSG") == 0 &&
                    pszAuthorityCode != NULL) {
                info->srid = atoi(pszAuthorityCode);
            }
        }
        OSRDestroySpatialReference(hSRS);
    }

    //S2 Set info record geotransform metrix
    if(GDALGetGeoTransform(hds, info->gt) != CE_None) {
        elog(DEBUG2, "Using default geotransform matrix (0, 1, 0, 0, 0, -1) for raster: %s", filename);
        info->gt[0] = 0;
        info->gt[1] = 1;
        info->gt[2] = 0;
        info->gt[3] = 0;
        info->gt[4] = 0;
        info->gt[5] = -1;
    }
    memcpy(gt, info->gt, sizeof(double) * 6);

    /*
     * TODO
     * Processing multiple bands
     */
    nbands = GDALGetRasterCount(hds);
    Assert(nbands == 1);
    info->nband_count = nbands;
    info->nband = rtalloc(info->nband_count * sizeof(int));
    if (info->nband == NULL) {
        GDALClose(hds);
        elog(ERROR, "rtalloc info->nband failed");
    }
    for (i = 0; i < info->nband_count; i++)
        info->nband[i] = i + 1;

    /* initialize parameters dependent on nband */
	info->gdalbandtype = rtalloc(sizeof(GDALDataType) * info->nband_count);
	if (info->gdalbandtype == NULL) {
		GDALClose(hds);
		elog(ERROR, "convert_raster: Could not allocate memory for storing GDAL data type");
	}
	info->bandtype = rtalloc(sizeof(rt_pixtype) * info->nband_count);
	if (info->bandtype == NULL) {
		GDALClose(hds);
		elog(ERROR, "convert_raster: Could not allocate memory for storing pixel type");
	}
    info->hasnodata = rtalloc(sizeof(int) * info->nband_count);
    if (info->hasnodata == NULL) {
	    GDALClose(hds);
	    elog(ERROR, "convert_raster: Could not allocate memory for storing hasnodata flag");
    }
    info->nodataval = rtalloc(sizeof(double) * info->nband_count);
    if (info->nodataval == NULL) {
	    GDALClose(hds);
	    elog(ERROR, "convert_raster: Could not allocate memory for storing nodata value");
    }

    memset(info->gdalbandtype, GDT_Unknown, sizeof(GDALDataType) * info->nband_count);
	memset(info->bandtype, PT_END, sizeof(rt_pixtype) * info->nband_count);
    memset(info->hasnodata, 0, sizeof(int) * info->nband_count);
    memset(info->nodataval, 0, sizeof(double) * info->nband_count);

    /* Process each band data type*/
    for (i = 0; i < info->nband_count; i++) {
        GDALRasterBandH rbh = GDALGetRasterBand(hds, info->nband[i]);
        info->gdalbandtype[i] = GDALGetRasterDataType(rbh);
        info->bandtype[i] = rt_util_gdal_datatype_to_pixtype(info->gdalbandtype[i]);

        /* hasnodata and nodataval*/
        info->nodataval[i] = GDALGetRasterNoDataValue(rbh, &(info->hasnodata[i]));
        if (!info->hasnodata[i]) {
            if(config->hasnodata) {
                info->hasnodata[i] = 1;
                info->nodataval[i] = config->nodataval;
            } else {
                info->nodataval[i] = 0;
            }
        }
    }

    elog(DEBUG1, "INFO Process each tile %d",__LINE__);
    /* Process each tile */
    for (xtile = cur_lineno / ntiles[1];  xtile < ntiles[0] && processdno < batchsize; xtile++) {
        // x coordinate edge
        if (!config->pad_tile && xtile == ntiles[0] - 1)
            _tile_size[0] = info->dim[0] - xtile * info->tile_size[0];
        else
            _tile_size[0] = info->tile_size[0];

        if(xtile == cur_lineno / ntiles[1])
            ytile = cur_lineno % ntiles[0];
        else
            ytile = 0;

        for (; ytile < ntiles[1]; ytile++) {
            // y coordinate edge
            if (!config->pad_tile && ytile == ntiles[1] - 1)
                _tile_size[1] = info->dim[1] - ytile * info->tile_size[1];
            else
                _tile_size[1] = info->tile_size[1];

            //Create tile
            tds = VRTCreate(_tile_size[0], _tile_size[1]);
            GDALSetProjection(tds, info->srs);

            //TODO: DO not understand why
            GDALApplyGeoTransform(info->gt, xtile * info->tile_size[0], ytile * info->tile_size[1], &(gt[0]),&(gt[3]));
            GDALSetGeoTransform(tds, gt);

            //Add band data sources
            elog(DEBUG1, "xtile=%d,ytile=%d,info->tile_size=%dx%d,_tile_size=%dx%d",xtile,ytile,info->tile_size[0],
			info->tile_size[1],_tile_size[0],_tile_size[1]);
            for (i = 0; i < info->nband_count; i++) {
                GDALAddBand(tds, info->gdalbandtype[i], NULL);
                tband = (VRTSourcedRasterBandH)GDALGetRasterBand(tds, i + 1);
                if (info->hasnodata[i])
                    GDALSetRasterNoDataValue(tband, info->nodataval[i]);
                VRTAddSimpleSource(tband, GDALGetRasterBand(hds, info->nband[i]),
                                   xtile * info->tile_size[0],ytile * info->tile_size[1],
                                   _tile_size[0],_tile_size[1],
                                   0,0,
                                   _tile_size[0],_tile_size[1],
                                    "near", VRT_NODATA_UNSET);

            }

            VRTFlushCache(tds);

            /* Convert VRT to raster to hexwkb
             */
            rast = rt_raster_from_gdal_dataset(tds);
            if (rast == NULL) {
                elog(ERROR, "rt_raster_from_gdal_dataset failed.");
                rterror("error");
                GDALClose(hds);
                return 0;
            }

            rt_raster_set_srid(rast, info->srid);

            hex = rt_raster_to_hexwkb(rast, FALSE, &hexlen);
            raster_destroy(rast);

            if (hex == NULL) {
                GDALClose(hds);
                elog(ERROR, "rt_raster_to_hexwk return NULL.");
            }
            GDALClose(tds);

            buf[processdno++] = hex;
            if(processdno == batchsize)
                break;
        }
    }// finish process all the tiles

    GDALClose(hds);
    elog(DEBUG1, "<----->convert_raster");
    return processdno;
}
