// Environment graph from mariadb
// Copyright (c) 2019 Adrian Kennard, Andrews & Arnold Limited, see LICENSE file (GPL)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <popt.h>
#include <err.h>
#include <curl/curl.h>
#include <sqllib.h>
#include <axl.h>
#include <math.h>

int debug = 0;

int
main (int argc, const char *argv[])
{
   const char *sqlhostname = NULL;
   const char *sqldatabase = "env";
   const char *sqlusername = NULL;
   const char *sqlpassword = NULL;
   const char *sqlconffile = NULL;
   const char *sqltable = "env";
   const char *tag = NULL;
   char *date = NULL;
   double xsize = 36;
   double ysize = 36;
   double tempstep = 1;
   double co2step = 50;
   double rhstep = 3;
   double co2base = 400;
   double tempbase = 0;
   double rhbase = 0;
   int debug = 0;
   int days = 1;
   {                            // POPT
      poptContext optCon;       // context for parsing command-line options
      const struct poptOption optionsTable[] = {
         {"sql-conffile", 'c', POPT_ARG_STRING, &sqlconffile, 0, "SQL conf file", "filename"},
         {"sql-hostname", 'H', POPT_ARG_STRING, &sqlhostname, 0, "SQL hostname", "hostname"},
         {"sql-database", 'd', POPT_ARG_STRING | POPT_ARGFLAG_SHOW_DEFAULT, &sqldatabase, 0, "SQL database", "db"},
         {"sql-username", 'U', POPT_ARG_STRING, &sqlusername, 0, "SQL username", "name"},
         {"sql-password", 'P', POPT_ARG_STRING, &sqlpassword, 0, "SQL password", "pass"},
         {"sql-table", 't', POPT_ARG_STRING | POPT_ARGFLAG_SHOW_DEFAULT, &sqltable, 0, "SQL table", "table"},
         {"sql-debug", 'v', POPT_ARG_NONE, &sqldebug, 0, "SQL Debug"},
         {"x-size", 0, POPT_ARG_DOUBLE | POPT_ARGFLAG_SHOW_DEFAULT, &xsize, 0, "X size per hour", "pixels"},
         {"y-size", 0, POPT_ARG_DOUBLE | POPT_ARGFLAG_SHOW_DEFAULT, &ysize, 0, "Y size per step", "pixels"},
         {"temp-step", 0, POPT_ARG_DOUBLE | POPT_ARGFLAG_SHOW_DEFAULT, &tempstep, 0, "Temp per Y step", "Celsius"},
         {"co2-step", 0, POPT_ARG_DOUBLE | POPT_ARGFLAG_SHOW_DEFAULT, &co2step, 0, "CO₂ per Y step", "ppm"},
         {"rh-step", 0, POPT_ARG_DOUBLE | POPT_ARGFLAG_SHOW_DEFAULT, &rhstep, 0, "RH per Y step", "%"},
         {"temp-base", 0, POPT_ARG_DOUBLE | POPT_ARGFLAG_SHOW_DEFAULT, &tempbase, 0, "Temp base", "Celsius"},
         {"co2-base", 0, POPT_ARG_DOUBLE | POPT_ARGFLAG_SHOW_DEFAULT, &co2base, 0, "CO₂ base", "ppm"},
         {"rh-base", 0, POPT_ARG_DOUBLE | POPT_ARGFLAG_SHOW_DEFAULT, &rhbase, 0, "RH base", "%"},
         {"tag", 'i', POPT_ARG_STRING, &tag, 0, "Device ID", "tag"},
         {"date", 'D', POPT_ARG_STRING, &date, 0, "Date", "YYYY-MM-DD"},
         {"days", 'N', POPT_ARG_INT | POPT_ARGFLAG_SHOW_DEFAULT, &days, 0, "Days", "N"},
         {"debug", 'V', POPT_ARG_NONE, &debug, 0, "Debug"},
         POPT_AUTOHELP {}
      };

      optCon = poptGetContext (NULL, argc, argv, optionsTable, 0);
      //poptSetOtherOptionHelp (optCon, "");

      int c;
      if ((c = poptGetNextOpt (optCon)) < -1)
         errx (1, "%s: %s\n", poptBadOption (optCon, POPT_BADOPTION_NOALIAS), poptStrerror (c));

      if (poptPeekArg (optCon) || !tag)
      {
         poptPrintUsage (optCon, stderr, 0);
         return -1;
      }
      poptFreeContext (optCon);
   }
   SQL sql;
   sql_real_connect (&sql, sqlhostname, sqlusername, sqlpassword, sqldatabase, 0, NULL, 0, 1, sqlconffile);
   char *edate = NULL;
   {
      struct tm t;
      time_t now = time (0);
      if (date)
         now = xml_time (date);
      localtime_r (&now, &t);
      t.tm_mday -= days - 1;
      mktime (&t);
      asprintf (&date, "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
      t.tm_mday += days;
      mktime (&t);
      asprintf (&edate, "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
   }
   SQL_RES *res = sql_safe_query_store_free (&sql,
                                             sql_printf
                                             ("SELECT * FROM `%#S` WHERE `tag`=%#s AND `when`>=%#s AND `when`<%#s ORDER BY `when`",
                                              sqltable, tag, date, edate));
   enum
   {
      CO2,
      TEMP,
      RH,
      MAX,
   };
   struct data_s
   {
      const char *arg;
      const char *colour;
      xml_t g;
      char *path;
      size_t size;
      FILE *f;
      double min;
      double max;
      double scale;
      char m;
   } data[MAX] = {
    {arg: "co2", colour: "green", scale: ysize / co2step, min: co2base, max:co2base},
    {arg: "temp", colour: "red", scale: ysize / tempstep, min: tempbase, max:tempbase},
    {arg: "rh", colour: "blue", scale: ysize / rhstep, min: rhbase, max:rhbase},
   };
   int d;
   int day = 0;
   xml_t svg = xml_tree_new ("svg");
   xml_element_set_namespace (svg, xml_namespace (svg, NULL, "http://www.w3.org/2000/svg"));
   xml_t top = xml_element_add (svg, "g");
   xml_t grid = xml_element_add (top, "g");
   xml_t axis = xml_element_add (top, "g");
   for (d = 0; d < MAX; d++)
   {
      data[d].g = xml_element_add (top, "g");
      xml_add (data[d].g, "@stroke", data[d].colour);
      xml_add (data[d].g, "@fill", "none");
   }
   void sod (void)
   {
      for (d = 0; d < MAX; d++)
      {
         data[d].f = open_memstream (&data[d].path, &data[d].size);
         data[d].m = 'M';
      }
   }
   void eod (void)
   {
      day++;
      for (d = 0; d < MAX; d++)
      {
         fclose (data[d].f);
         xml_t p = xml_element_add (data[d].g, "path");
         xml_addf (p, "@opacity", "%.1f", (double) day / days);
         xml_add (p, "@d", data[d].path);
         free (data[d].path);
      }
   }
   int maxx = 0;
   sod ();
   time_t start = xml_time (date);
   while (sql_fetch_row (res))
   {
      const char *when = sql_col (res, "when");
      if (strncmp (date, when, 10))
      {                         // New day
         memcpy (date, when, 10);
         start = xml_time (date);
         eod ();
         sod ();
      }
      for (d = 0; d < MAX; d++)
      {
         const char *val = sql_col (res, data[d].arg);
         if (!val)
            continue;
         double v = strtod (val, NULL);
         if (data[d].min > v)
            data[d].min = v;
         if (data[d].max < v)
            data[d].max = v;
         int x = (xml_time (when) - start) * xsize / 3600;
         if (x > maxx)
            maxx = x;
         int y = v * data[d].scale;
         fprintf (data[d].f, "%c%d,%d", data[d].m, x, y);
         data[d].m = 'L';
      }
   }
   sql_free_result (res);
   sql_close (&sql);
   eod ();
   maxx = floor ((maxx + xsize - 1) / xsize) * xsize;
   int maxy = ysize * 30;
   // Normalise min and work out y size
   for (d = 0; d < MAX; d++)
   {

   }
   // Grid
   xml_add (grid, "@stroke", "black");
   xml_add (grid, "@fill", "none");
   xml_add (grid, "@opacity", "0.1");
   for (int y = 0; y <= maxy; y += ysize)
   {
      xml_t l = xml_element_add (grid, "path");
      xml_addf (l, "@d", "M0,%dL%d,%d", y, maxx, y);
   }
   for (int x = 0; x <= maxx; x += xsize)
   {
      xml_t l = xml_element_add (grid, "path");
      xml_addf (l, "@d", "M%d,0L%d,%d", x, x, maxy);
   }
   // Axis
   xml_add (axis, "@opacity", "0.5");
   xml_add (top, "@font-family", "sans-serif");
   for (d = 0; d < MAX; d++)
   {
	   // TODO move to axis once we have aligned
      xml_t g = xml_element_add (data[d].g, "g");
      xml_add (g, "@opacity", "0.5");
      for (int y = 0; y <= maxy; y += ysize)
      {
         double v = (double) y / data[d].scale + data[d].min;
         xml_t t = xml_addf (g, "+text", d == TEMP ? "%.1f" : "%.0f", v);
         xml_addf (t, "@x", "%d", d * 40 + 20);
         xml_addf (t, "@y", "%d", y);
         xml_add (t, "@alignment-baseline", "middle");
         xml_add (t, "@fill", data[d].colour);
      }
   }
   for (int x = 0; x <= maxx; x += xsize)
   {
      xml_t t = xml_addf (axis, "+text", "%02d", (int) (x / xsize) % 24);
      xml_addf (t, "@x", "%d", x);
      xml_addf (t, "@y", "%d", maxy);
      xml_add (axis, "@text-anchor", "middle");
   }
   xml_addf (svg, "@width", "%d", maxx + 1);
   xml_addf (svg, "@height", "%d", maxy + 1);
   // Position and invert
   for (d = 0; d < MAX; d++)
   {
      xml_addf (data[d].g, "@transform", "translate(0,%.1f)scale(1,-1)", data[d].scale * (data[d].min + data[d].max));
      // Axis
   }
   // Write out
   xml_write (stdout, svg);
   xml_tree_delete (svg);
   return 0;
}
