// math_util.h
float min(float a, float b, float c) {
	if (a < b && a < c) {
		return a;
	} else if (b < a && b < c) {
		return b;
	} else if (c < a && c < b) {
		return c;
	} else {
		return a;
	}
}

float max(float a, float b, float c) {
	if (a > b && a > c) {
		return a;
	} else if (b > a && b > c) {
		return b;
	} else if (c > a && c > b) {
		return c;
	} else if (a > b && a == c) {
		return a;
	} else if (a > c && a == b) {
		return a;
	} else if (b > a && b == c) {
		return b;
	} else if (b > c && b == a) {
		return b;
	} else {
		return c;
	}
}

double sgn(double a) {
	if (a>0)
        	return 1;
	else if (a<0)
        	return -1;
	else
	        return 0;
}

