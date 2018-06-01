/*-------------------------------------------------------------------------
 *
 * ogr_fdw_info.c
 *		Commandline utility to read an OGR layer and output a
 *		SQL "create table" statement.
 *
 * Copyright (c) 2014-2015, Paul Ramsey <pramsey@cleverelephant.ca>
 *
 *-------------------------------------------------------------------------
 */

/* getopt */
#include <unistd.h>
#include <stdbool.h>

/*
 * OGR library API
 */
#include "ogr_fdw_gdal.h"
#include "ogr_fdw_common.h"

static void usage();
static OGRErr ogrListLayers(const char *source);
static OGRErr ogrGenerateSQL(const char *source, const char *layer);
static OGRErr rasterGenerateSQL(const char *source, const char *conf);

#define STR_MAX_LEN 256


/* Define this no-op here, so that code */
/* in the ogr_fdw_common module works */
const char * quote_identifier(const char *ident);

const char *
quote_identifier(const char *ident)
{
	return ident;
}


static void
formats()
{
	int i;

	GDALAllRegister();

	printf( "Supported Formats:\n" );
	for ( i = 0; i < GDALGetDriverCount(); i++ )
	{
		GDALDriverH ogr_dr = GDALGetDriver(i);
		int vector = FALSE;
		int createable = TRUE;
		const char *tmpl;

#if GDAL_VERSION_MAJOR >= 2
		char** papszMD = GDALGetMetadata(ogr_dr, NULL);
		vector = CSLFetchBoolean(papszMD, GDAL_DCAP_VECTOR, FALSE);
		createable = CSLFetchBoolean(papszMD, GDAL_DCAP_CREATE, FALSE);
#else
		createable = GDALDatasetTestCapability(ogr_dr, ODrCCreateDataSource);
#endif
		/* Skip raster data sources */
		if ( ! vector &&  strcmp(GDALGetDriverShortName(ogr_dr), "GTiff")) 
		    	continue;

		/* Report sources w/ create capability as r/w */
		if( createable )
			tmpl = "  -> \"%s\" (read/write)\n";
		else
			tmpl = "  -> \"%s\" (readonly)\n";

		printf(tmpl, GDALGetDriverShortName(ogr_dr));
	}

	exit(0);
}

static void
usage()
{
	printf(
		"usage:    ogr_fdw_info -s <ogr datasource> -l <ogr layer>\n"
		"	   ogr_fdw_info -s <ogr datasource>\n"
		"	   ogr_fdw_info -s <raster datasource> -c <raster conf_file> -r\n"
		"	   ogr_fdw_info -f\n"
		"\n");
	exit(0);
}

int
main (int argc, char **argv)
{
	int ch;
	char *source = NULL, *layer = NULL;
	OGRErr err = OGRERR_NONE;
	char *conf = NULL;
	bool raster_flag = false;

	/* If no options are specified, display usage */
	if (argc == 1)
		usage();

	while ((ch = getopt(argc, argv, "h?rs:l:fc:")) != -1) {
		switch (ch) {
			case 's':
				source = optarg;
				break;
			case 'r':
				raster_flag = true;
				break;
			case 'c':
				conf = optarg;
				break;
			case 'l':
				layer = optarg;
				break;
			case 'f':
				formats();
				break;
			case '?':
			case 'h':
			default:
				usage();
				break;
		}
	}

	if ( source && ! layer && ! raster_flag)
	{
		err = ogrListLayers(source);
	}
	else if ( source && raster_flag && conf)
	{
		err = rasterGenerateSQL(source, conf);
	}
	else if ( source && layer )
	{
		err = ogrGenerateSQL(source, layer);
	}
	else if ( ! source && ! layer )
	{
		usage();
	}

	if ( err != OGRERR_NONE )
	{
		// printf("OGR Error: %s\n\n", CPLGetLastErrorMsg());
	}

	OGRCleanupAll();
	exit(0);
}

static OGRErr
ogrListLayers(const char *source)
{
	GDALDatasetH ogr_ds = NULL;
	int i;
	bool is_raster = false;
	GDALDriverH ogr_dr = NULL;

	GDALAllRegister();

#if GDAL_VERSION_MAJOR < 2
	ogr_ds = OGROpen(source, FALSE, NULL);
#else
	ogr_ds = GDALOpenEx(source,
						GDAL_OF_READONLY,
						//GDAL_OF_VECTOR|GDAL_OF_READONLY,
						NULL, NULL, NULL);
#endif

	ogr_dr = GDALGetDatasetDriver(ogr_ds);
	if (strcmp(GDALGetDriverShortName(ogr_dr), "GTiff") == 0)
	    is_raster = true;

	if ( ! ogr_ds )
	{
	    CPLError(CE_Failure, CPLE_AppDefined, "Could not connect to source '%s'", source);
	    return OGRERR_FAILURE;
	}

	if (!is_raster) {
	    printf("Layers:\n");
	    for ( i = 0; i < GDALDatasetGetLayerCount(ogr_ds); i++ )
	    {
		OGRLayerH ogr_lyr = GDALDatasetGetLayer(ogr_ds, i);
		if ( ! ogr_lyr )
		{
		    return OGRERR_FAILURE;
		}
		printf("  %s\n", OGR_L_GetName(ogr_lyr));
	    }
	} else {
	    printf("This is a raster GTiff file, Please specify a conf_file with -c");
	}
	printf("\n");

	GDALClose(ogr_ds);

	return OGRERR_NONE;
}

static OGRErr
ogrGenerateSQL(const char *source, const char *layer)
{
	OGRErr err;
	GDALDatasetH ogr_ds = NULL;
	GDALDriverH ogr_dr = NULL;
	OGRLayerH ogr_lyr = NULL;
	char server_name[STR_MAX_LEN];
	stringbuffer_t buf;

	GDALAllRegister();

#if GDAL_VERSION_MAJOR < 2
	ogr_ds = OGROpen(source, FALSE, &ogr_dr);
#else
	ogr_ds = GDALOpenEx(source,
						GDAL_OF_VECTOR|GDAL_OF_READONLY,
						NULL, NULL, NULL);
#endif

	if ( ! ogr_ds )
	{
		CPLError(CE_Failure, CPLE_AppDefined, "Could not connect to source '%s'", source);
		return OGRERR_FAILURE;
	}

	if ( ! ogr_dr )
		ogr_dr = GDALGetDatasetDriver(ogr_ds);

	/* There should be a nicer way to do this */
	strcpy(server_name, "myserver");

	ogr_lyr = GDALDatasetGetLayerByName(ogr_ds, layer);
	if ( ! ogr_lyr )
	{
		CPLError(CE_Failure, CPLE_AppDefined, "Could not find layer '%s' in source '%s'", layer, source);
		return OGRERR_FAILURE;
	}

	/* Output SERVER definition */
	printf("\nCREATE SERVER %s\n"
		"  FOREIGN DATA WRAPPER ogr_fdw\n"
		"  OPTIONS (\n"
		"	datasource '%s',\n"
		"	format '%s' );\n",
		server_name, source, GDALGetDriverShortName(ogr_dr));

	stringbuffer_init(&buf);
	err = ogrLayerToSQL(ogr_lyr,
			server_name,
			TRUE, /* launder table names */
			TRUE, /* launder column names */
			TRUE, /* use postgis geometry */
			&buf);

	GDALClose(ogr_ds);

	if ( err != OGRERR_NONE )
	{
		return err;
	}

	printf("\n%s\n", stringbuffer_getstring(&buf));
	stringbuffer_release(&buf);
	return OGRERR_NONE;
}

static OGRErr
rasterGenerateSQL(const char *source, const char *conf)
{
	char server_name[STR_MAX_LEN];
	char table_name[STR_MAX_LEN];

	/* There should be a nicer way to do this */
	strcpy(server_name, "myserver");
	strcpy(table_name, "mytable");

	/* Output SERVER definition */
	printf("\nCREATE SERVER %s\n"
		"  FOREIGN DATA WRAPPER ogr_fdw\n"
		"  OPTIONS (\n"
		"	datasource '%s',\n"
		"	format 'GTiff' );\n",
		server_name, source);//, GDALGetDriverShortName(rt_dr));

	//create foreign table raster_test (rast raster) server rasterServer options(conf_file '/home/hewenting/casearth/rasterdb/etc/rasterdb.conf'
	printf("\nCREATE FOREIGN TABLE %s(rast raster)\n"
		"  SERVER %s\n"
		"  OPTIONS (\n"
		"	conf_file '%s' );\n",
		table_name, server_name, conf);

	return OGRERR_NONE;
}
