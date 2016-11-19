select * from uao_visimap_test06;
select count(*) from gp_dist_random('pg_aoseg.pg_aovisimap_18663');
insert into uao_visimap_test06 select i,'aa'||i,i+10 from generate_series(1,5) as i;
delete from uao_visimap_test06 where i=3;
select * from uao_visimap_test06;
select count(*) from gp_dist_random('pg_aoseg.pg_aovisimap_18663');
