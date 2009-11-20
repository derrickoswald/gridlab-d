#include "csv_reader.h"

CLASS *csv_reader::oclass = 0;

EXPORT int create_csv_reader(OBJECT **obj, OBJECT *parent){
	*obj = gl_create_object(csv_reader::oclass);
	if(*obj != NULL){
		
		return 1;
	}
	//printf("create_csv_reader\n");
	return 0;	// don't want it to get called, but better to have it not be fatal
}

EXPORT int init_csv_reader(OBJECT **obj, OBJECT *parent){
	csv_reader *my = OBJECTDATA(obj,csv_reader);
	return 1; // let the climate object cause the file to open
}

/// Synchronize the cliamte object
EXPORT TIMESTAMP sync_csv_reader(OBJECT *obj, TIMESTAMP t0){
	return TS_NEVER; // really doesn't do anything
}

csv_reader::csv_reader(){
	;
}

csv_reader::csv_reader(MODULE *module){
	memset(this, 0, sizeof(csv_reader));
	if (oclass==NULL)
	{
		oclass = gl_register_class(module,"csv_reader",sizeof(csv_reader),NULL);
		if (gl_publish_variable(oclass,
			PT_int32,"index",PADDR(index),PT_ACCESS,PA_REFERENCE,
			PT_char32,"city_name",PADDR(city_name),
			PT_char32,"state_name",PADDR(state_name),
			PT_double,"lat_deg",PADDR(lat_deg),
			PT_double,"lat_min",PADDR(lat_min),
			PT_double,"long_deg", PADDR(long_deg),
			PT_double,"long_min",PADDR(long_min),
			PT_double,"low_temp",PADDR(low_temp),PT_ACCESS,PA_REFERENCE,
			PT_double,"high_temp",PADDR(high_temp),PT_ACCESS,PA_REFERENCE,
			PT_double,"peak_solar",PADDR(peak_solar),PT_ACCESS,PA_REFERENCE,
			PT_enumeration,"status",PADDR(status),PT_ACCESS,PA_REFERENCE,
				PT_KEYWORD,"INIT",CR_INIT,
				PT_KEYWORD,"OPEN",CR_OPEN,
				PT_KEYWORD,"ERROR",CR_ERROR,
			PT_char32,"timefmt",PADDR(timefmt),
			PT_char32,"timezone",PADDR(timezone),
			PT_char256,"columns",PADDR(columns_str),
			PT_char256,"filename",PADDR(filename),
			NULL)<1) GL_THROW("unable to publish properties in %s",__FILE__);
		memset(this,0,sizeof(csv_reader));
	}
}

/**
	Open a CSV file and parse it as
 **/
int csv_reader::open(const char *file){
	char line[1024];
	char filename[128];
	int has_cols = 0;
	int linenum = 0;
	int i = 0;
	weather *wtr = 0;

	if(file == 0){
		gl_error("csv_reader has no input file name!");
		return 0;
	}

	strncpy(filename, file, 127);
	infile = fopen(filename, "r");
	if(infile == 0){
		gl_error("csv_reader could not open \'%s\' for input!", file);
		return 0;
	}

	if(columns_str[0] != 0){
		if(0 == read_header(columns_str)){
			gl_error("csv_reader::open ~ column header read failure from explicit headers");
			return 0;
		}
	}
	while(fgets(line, 1024, infile) > 0){
		++linenum;
		if(line[0] == '#'){	// comment
			continue;
		}
		else if(line[0] == '$'){	// property
			if(0 == read_prop(line+1)){
				gl_error("csv_reader::open ~ property read failure on line %i", linenum);
				return 0;
			} else {
				continue;
			}
		}
		else if(has_cols == 0){
			if(0 == read_header(line)){
				gl_error("csv_reader::open ~ column header read failure on line %i", linenum);
				return 0;
			} else {
				has_cols = 1;
			}
		} else {
			if(0 == read_line(line)){
				gl_error("csv_reader::open ~ data line read failure on line %i", linenum);
				return 0;
			} else {
				++sample_ct;
			}
		}
	}

	/* move list into double pointer */
	samples = (weather**)malloc(sizeof(weather *) * (size_t) sample_ct);
	for(i = 0, wtr = weather_root; i < sample_ct && wtr != NULL; ++i, wtr=wtr->next){
		samples[i] = wtr;
	}

//	index = -1;	// forces to start on zero-eth index

	return 1;
}

int csv_reader::read_prop(char *line){ // already pulled the '$' off the front
	OBJECT *my = OBJECTHDR(this);
	char *split = strchr(line, '=');
	char propstr[256], valstr[256];
	PROPERTY *prop = 0;

	if(split == NULL){
		gl_error("csv_reader::read_prop ~ missing \'=\' seperator");
		return 0;
	}

	if(2 != sscanf(line, "%[^=]=%[^\n]", propstr, valstr)){
		gl_error("csv_reader::read_prop ~ error reading property & value");
		return 0;
	}
	
	prop = gl_find_property(oclass, propstr);
	if(prop == 0){
		gl_error("csv_reader::read_prop ~ unrecognized csv_reader property \'%s\'", propstr);
		return 0;
	}

	// Windows pointers on my Dell Precision 390 are apparently ulli -MH
	/* BAD BAD BAD!  cannot use 'my' with this, it may not have an associate object header!!! */
//	if(0 == gl_set_value(my, (void *)((unsigned long long int)this + (unsigned long long int)prop->addr), valstr, prop)){
//		gl_error("csv_reader::read_prop ~ unable to set property \'%s\' to \'%s\'", propstr, valstr);
//		return 0;
//	}	
	void *addr = ((unsigned long long int)this + (unsigned long long int)prop->addr);
	if(prop->ptype == PT_double){
		if(1 != sscanf(valstr, "%lg", addr)){
			gl_error("csv_reader::read_prop ~ unable to set property \'%s\' to \'%s\'", propstr, valstr);
			return 0;
		}
	} else if(prop->ptype == PT_char32){
		strncpy((char *)addr, valstr, 32);
	} else {
		gl_error("csv_reader::read_prop ~ unable to convert property \'%s\' due to type restrictions", propstr);
		/* TROUBLESHOOTING
			This is a programming problem.  The property parser within the csv_reader is only able to
			properly handle char32 and double properties.
		 */
		return 0;
	}
	return 1;
}

int csv_reader::read_header(char *line){
	struct cmnlist {
		char *name;
		PROPERTY *column;
		struct cmnlist *next;
	};
	char buffer[1024];
	int index = 0, start_idx = 0;
	int done = 0;
	int i = 0;
	PROPERTY *prop = 0;
	struct cmnlist *first = 0, *last = 0, *temp = 0;

	// expected format: x,y,z\n

	memset(buffer, 0, 1024);
	strncpy(buffer, line, 1023);

	// split column header list
	while(index < 1024 && 0 == done){
		while(buffer[index] != 0 && buffer[index] != ',' && buffer[index] != '\n' && buffer[index] != '\r'){
			++index;
		}
		if(buffer[index] == ','){
			buffer[index] = 0;
			++index;
		}
		if(buffer[index] == '\n' || buffer[index] == '\r')
			buffer[index] = 0;

		temp = (struct cmnlist *)malloc(sizeof(struct cmnlist));
		temp->name = buffer+start_idx;
		temp->column = prop;
		temp->next = 0;

		start_idx = index;
		++column_ct;

		if(first == 0){
			first = last = temp;
		} else {
			last->next = temp;
			last = temp;
		}

		if(buffer[index] == 0 || buffer[index] == '\n' || buffer[index] == '\r'){
			done = 1;
			break;
		}
	}

	//	find properties for each column header
	temp = first;
	columns = (PROPERTY **)malloc(sizeof(PROPERTY *) * (size_t)column_ct);
	while(temp != 0 && i < column_ct){
		temp->column = gl_find_property(weather::oclass, temp->name);
		if(temp->column == 0){
			gl_error("csv_reader::read_header ~ unable to find column property \'%s\''", temp->name);
			return 0;
		}
		columns[i] = temp->column;
		temp = temp->next;
		++i;
	}
	return 1;
}

int csv_reader::read_line(char *line){
	int done = 0;
	int col = 0;
	char buffer[1024];
	char *token = 0;
	weather *sample = new weather();
//	OBJECT *my = 0;

	if(weather_root == 0){
		weather_root = sample;
	} else {
		weather_last->next = sample;
	}
	weather_last = sample;
	
	strncpy(buffer, line, 1023);

	token = strtok(buffer, ",\n\r");
	if(timefmt[0] == 0){
		if(sscanf(token, "%i:%i:%i:%i:%i", &sample->month, &sample->day, &sample->hour, &sample->minute, &sample->second) < 1){
			gl_error("csv_reader::read_line ~ unable to read time string \'%s\' with default format", token);
			return 0;
		}
	} else {
		if(sscanf(token, timefmt, &sample->month, &sample->day, &sample->hour, &sample->minute, &sample->second) < 1){
			gl_error("csv_reader::read_line ~ unable to read time string \'%s\' with format \'%s\'", token, timefmt);
			return 0;
		}
	}

	// should gracefully discard sample if time "goes backwards"

	while((token=strtok(NULL, ",\n\r")) != 0 && col < column_ct){
		if(columns[col]->ptype == PT_double){
			double *dptr = (double *)((unsigned long long int)(columns[col]->addr) + (unsigned long long int)(sample));
			if(sscanf(token, "%lg", dptr) != 1){
				gl_error("csv_reader::read_line ~ unable to set value \'%s\' to double property \'%s\'", token, columns[col]->name);
				return 0;
			}
		}
		++col;
	}
	return 1;
}

TIMESTAMP csv_reader::get_data(TIMESTAMP t0, double *temp, double *humid, double *direct, double *diffuse, double *wind, double *rain, double *snow){
	DATETIME now, then;
//	TIMESTAMP until;
	int next_year = 0;
	int i = 0;
	int idx = index;
	int start = index;

	int localres;

	if(t0 < next_ts){ /* still good ~ go home */
		return -next_ts;
	}

	localres = gl_localtime(t0, &now); // error check

	if(next_ts == 0){
		//	initialize to the correct index & next_ts
		DATETIME guess_dt;
		TIMESTAMP guess_ts;
		int i;
		for(i = 0; i < sample_ct; ++i){
			guess_dt.year = now.year;
			guess_dt.month = samples[sample_ct-i-1]->month;
			guess_dt.day = samples[sample_ct-i-1]->day;
			guess_dt.hour = samples[sample_ct-i-1]->hour;
			guess_dt.minute = samples[sample_ct-i-1]->minute;
			guess_dt.second = samples[sample_ct-i-1]->second;
			guess_dt.tz[0] = 0;
			guess_ts = (TIMESTAMP)gl_mktime(&guess_dt);

			if(guess_ts < t0){
				break;
			}
		}
		index = sample_ct - i - 1;

		if(index > -1){
			*temp = samples[index]->temperature;
			*humid = samples[index]->humidity;
			*direct = samples[index]->solar_dir;
			*diffuse = samples[index]->solar_diff;
			*wind = samples[index]->wind_speed;
			*rain = samples[index]->rainfall;
			*snow = samples[index]->snowdepth;
		} else {
			*temp = samples[sample_ct - 1]->temperature;
			*humid = samples[sample_ct - 1]->humidity;
			*direct = samples[sample_ct - 1]->solar_dir;
			*diffuse = samples[sample_ct - 1]->solar_diff;
			*wind = samples[sample_ct - 1]->wind_speed;
			*rain = samples[sample_ct - 1]->rainfall;
			*snow = samples[sample_ct - 1]->snowdepth;
		}

		then.year = now.year + (index+1 == sample_ct ? 1 : 0);
		then.month = samples[(index+1)%sample_ct]->month;
		then.day = samples[(index+1)%sample_ct]->day;
		then.hour = samples[(index+1)%sample_ct]->hour;
		then.minute = samples[(index+1)%sample_ct]->minute;
		then.second = samples[(index+1)%sample_ct]->second;
		then.tz[0] = 0;

		next_ts = (TIMESTAMP)gl_mktime(&then);

		return -next_ts;
	}

	if(sample_ct == 1){ /* only one sample ~ ignore it and keep feeding the same data back, but in a year */
		next_ts += 365 * 24 * 3600;
		return -next_ts;
	}

	do{
		// should we roll the year over?
		if(index+1 >= sample_ct){
			index = 0;
		} else {
			++index;
		}

		if(index+1 == sample_ct){
			next_year = 1;
		} else {
			next_year = 0;
		}

		then.year = now.year + next_year;
		then.month = samples[(index+1)%sample_ct]->month;
		then.day = samples[(index+1)%sample_ct]->day;
		then.hour = samples[(index+1)%sample_ct]->hour;
		then.minute = samples[(index+1)%sample_ct]->minute;
		then.second = samples[(index+1)%sample_ct]->second;
		then.tz[0] = 0;

		// next_ts is the time the current sample is overwritten by another sample.
		next_ts = (TIMESTAMP)gl_mktime(&then);
	} while (next_ts < t0 && index != start); // skip samples that try to reverse the time
	
	*temp = samples[index]->temperature;
	*humid = samples[index]->humidity;
	*direct = samples[index]->solar_dir;
	*diffuse = samples[index]->solar_diff;
	*wind = samples[index]->wind_speed;
	*rain = samples[index]->rainfall;
	*snow = samples[index]->snowdepth;

	// having found the index, update the data
	if(index == start){
		GL_THROW("something strange happened with the schedule in csv_reader");
	}
	
	return -next_ts;
}

// EOF